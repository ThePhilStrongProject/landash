/*
 * NetBIOS name service discovery.
 *
 * A node-status query to UDP 137 makes a Windows machine (and anything running
 * Samba) hand back its own name table. It is the last source to run because it
 * only helps hosts that nothing else named, but for a Windows desktop that
 * advertises no mDNS and has no DHCP lease name it is often the only one that
 * answers.
 *
 * The reply is parsed defensively: every offset is checked against the
 * received length before it is read.
 */
#include "disc_nbns.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "device_db.h"
#include "disc_mdns.h"

static const char *TAG = "disc_nbns";

#define NBNS_PORT          137
#define NBNS_MAX_QUERIES   32
#define NBNS_TIMEOUT_MS    300
#define NBNS_GAP_MS        30
#define NBNS_BUF_CAP       512

#define NBNS_HEADER_LEN    12
#define NBNS_TYPE_NBSTAT   0x0021
#define NBNS_CLASS_IN      0x0001

/* Each name-table entry is 15 name bytes, a suffix byte and two flag bytes. */
#define NBNS_ENTRY_LEN     18
#define NBNS_NAME_LEN      15
#define NBNS_FLAG_GROUP    0x8000

/* Suffixes worth taking: 0x00 workstation, 0x20 file server. */
#define NBNS_SUFFIX_WKSTA  0x00
#define NBNS_SUFFIX_SERVER 0x20

static uint8_t s_tx[NBNS_BUF_CAP];
static uint8_t s_rx[NBNS_BUF_CAP];

/* ------------------------------------------------------------------------- */
/* Wire format                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Builds a node-status request for the wildcard name. The NetBIOS first-level
 * encoding splits each of the 16 name bytes into two nibbles and adds 'A', so
 * "*" padded with NULs becomes the 32-character label CKAAAA...A.
 */
static int nbns_build_query(uint8_t *buf, size_t cap, uint16_t id)
{
    if (cap < 50) {
        return -1;
    }

    size_t p = 0;
    buf[p++] = (uint8_t)(id >> 8);
    buf[p++] = (uint8_t)(id & 0xff);
    buf[p++] = 0x00; buf[p++] = 0x00;   /* query, no recursion desired       */
    buf[p++] = 0x00; buf[p++] = 0x01;   /* QDCOUNT 1                         */
    buf[p++] = 0x00; buf[p++] = 0x00;   /* ANCOUNT                           */
    buf[p++] = 0x00; buf[p++] = 0x00;   /* NSCOUNT                           */
    buf[p++] = 0x00; buf[p++] = 0x00;   /* ARCOUNT                           */

    static const uint8_t wildcard[16] = { '*' };    /* rest is NUL padding   */

    buf[p++] = 32;                                  /* encoded label length  */
    for (size_t i = 0; i < sizeof(wildcard); i++) {
        buf[p++] = (uint8_t)('A' + (wildcard[i] >> 4));
        buf[p++] = (uint8_t)('A' + (wildcard[i] & 0x0f));
    }
    buf[p++] = 0x00;                                /* root label            */

    buf[p++] = (uint8_t)(NBNS_TYPE_NBSTAT >> 8);
    buf[p++] = (uint8_t)(NBNS_TYPE_NBSTAT & 0xff);
    buf[p++] = (uint8_t)(NBNS_CLASS_IN >> 8);
    buf[p++] = (uint8_t)(NBNS_CLASS_IN & 0xff);
    return (int)p;
}

/* Steps over a name. NBNS replies are not compressed, but pointers are legal. */
static int nbns_skip_name(const uint8_t *buf, int len, int pos)
{
    while (pos >= 0 && pos < len) {
        const uint8_t l = buf[pos];
        if (l == 0) {
            return pos + 1;
        }
        if ((l & 0xc0) == 0xc0) {
            return (pos + 2 <= len) ? pos + 2 : -1;
        }
        if ((l & 0xc0) != 0) {
            return -1;
        }
        pos += 1 + l;
    }
    return -1;
}

/*
 * Extracts the machine name from a node-status reply: the first unique entry
 * with a workstation or server suffix.
 */
static bool nbns_parse_name(const uint8_t *buf, int len, uint16_t id,
                            char *out, size_t out_cap)
{
    if (len < NBNS_HEADER_LEN || out_cap <= NBNS_NAME_LEN) {
        return false;
    }
    if (((buf[0] << 8) | buf[1]) != id) {
        return false;
    }
    if ((buf[2] & 0x80) == 0) {
        return false;                   /* not a response                    */
    }

    const int qd = (buf[4] << 8) | buf[5];
    const int an = (buf[6] << 8) | buf[7];
    if (an < 1) {
        return false;
    }

    int pos = NBNS_HEADER_LEN;
    for (int i = 0; i < qd; i++) {
        pos = nbns_skip_name(buf, len, pos);
        if (pos < 0 || pos + 4 > len) {
            return false;
        }
        pos += 4;
    }

    /* Only the first answer carries the name table. */
    pos = nbns_skip_name(buf, len, pos);
    if (pos < 0 || pos + 11 > len) {
        return false;
    }

    const int type     = (buf[pos] << 8) | buf[pos + 1];
    const int rdlength = (buf[pos + 8] << 8) | buf[pos + 9];
    pos += 10;

    if (type != NBNS_TYPE_NBSTAT || rdlength < 1 || pos + rdlength > len) {
        return false;
    }

    const int entries = buf[pos];
    pos += 1;
    if (pos + entries * NBNS_ENTRY_LEN > len) {
        return false;
    }

    for (int i = 0; i < entries; i++) {
        const uint8_t *e      = buf + pos + (i * NBNS_ENTRY_LEN);
        const uint8_t  suffix = e[NBNS_NAME_LEN];
        const uint16_t flags  = (uint16_t)((e[16] << 8) | e[17]);

        if ((flags & NBNS_FLAG_GROUP) != 0) {
            continue;                   /* workgroup, not a machine          */
        }
        if (suffix != NBNS_SUFFIX_WKSTA && suffix != NBNS_SUFFIX_SERVER) {
            continue;
        }

        size_t o = 0;
        for (int c = 0; c < NBNS_NAME_LEN; c++) {
            const unsigned char ch = e[c];
            if (ch >= 0x20 && ch < 0x7f) {
                out[o++] = (char)ch;
            }
        }
        while (o > 0 && out[o - 1] == ' ') {
            o--;                        /* names are space padded            */
        }
        out[o] = '\0';

        if (out[0] != '\0') {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Pass                                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t disc_nbns_run_once(void)
{
    const disc_snap_t *snap  = NULL;
    const size_t       count = disc_snapshot_acquire(&snap);
    if (count == 0) {
        disc_snapshot_release();
        ESP_LOGD(TAG, "nothing online, skipping");
        return ESP_OK;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        disc_snapshot_release();
        ESP_LOGW(TAG, "socket failed: errno %d", errno);
        return ESP_FAIL;
    }

    const struct timeval tv = {
        .tv_sec  = NBNS_TIMEOUT_MS / 1000,
        .tv_usec = (NBNS_TIMEOUT_MS % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int queries = 0;
    int named   = 0;

    for (size_t i = 0; i < count && queries < NBNS_MAX_QUERIES; i++) {
        /* Last resort only: anything with any name at all is left alone. */
        if (snap[i].name_src != NETDASH_NAME_SRC_NONE) {
            continue;
        }

        const uint16_t id  = (uint16_t)(esp_random() & 0xffff);
        const int      len = nbns_build_query(s_tx, sizeof(s_tx), id);
        if (len < 0) {
            break;
        }

        struct sockaddr_in dst = {
            .sin_family = AF_INET,
            .sin_port   = htons(NBNS_PORT),
        };
        dst.sin_addr.s_addr = htonl(snap[i].ip);

        queries++;
        if (sendto(sock, s_tx, (size_t)len, 0,
                   (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            ESP_LOGD(TAG, "sendto failed: errno %d", errno);
            continue;
        }

        const int n = recv(sock, s_rx, sizeof(s_rx), 0);
        if (n > 0) {
            char name[NBNS_NAME_LEN + 1];
            if (nbns_parse_name(s_rx, n, id, name, sizeof(name))) {
                (void)device_db_set_hostname(snap[i].mac, name, NETDASH_NAME_SRC_NBNS);
                (void)device_db_set_services(snap[i].mac, NETDASH_SVC_WORKSTATION);
                named++;
                ESP_LOGD(TAG, "%u.%u.%u.%u -> %s",
                         (unsigned)((snap[i].ip >> 24) & 0xff),
                         (unsigned)((snap[i].ip >> 16) & 0xff),
                         (unsigned)((snap[i].ip >> 8) & 0xff),
                         (unsigned)(snap[i].ip & 0xff), name);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(NBNS_GAP_MS));
    }

    close(sock);
    disc_snapshot_release();

    ESP_LOGI(TAG, "nbns: %d queries, %d named", queries, named);
    return ESP_OK;
}

esp_err_t disc_nbns_init(void)
{
    return disc_task_ensure();
}
