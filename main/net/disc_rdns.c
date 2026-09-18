/*
 * Reverse-DNS discovery.
 *
 * ASUS routers (and most consumer routers) run dnsmasq, which answers PTR
 * queries for its own DHCP leases with the name the client asked for. On a
 * typical home network that is the single best source of names, because it
 * covers phones and laptops that advertise nothing at all.
 *
 * lwIP exposes no PTR API, so this is a minimal DNS client: query builder,
 * bounded name decoder with compression-pointer following, and an answer walk.
 * Everything is length-checked against the received size; a malformed reply is
 * dropped at DEBUG rather than trusted.
 */
#include "disc_rdns.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "device_db.h"
#include "disc_mdns.h"
#include "wifi_mgr.h"

static const char *TAG = "disc_rdns";

#define RDNS_PORT            53
#define RDNS_MAX_QUERIES     64
#define RDNS_TIMEOUT_MS      400
#define RDNS_GAP_MS          50
#define RDNS_BUF_CAP         512
#define RDNS_MAX_JUMPS       10

#define DNS_TYPE_PTR         12
#define DNS_CLASS_IN         1
#define DNS_HEADER_LEN       12

static uint8_t s_tx[RDNS_BUF_CAP];
static uint8_t s_rx[RDNS_BUF_CAP];

/* ------------------------------------------------------------------------- */
/* Wire format                                                               */
/* ------------------------------------------------------------------------- */

/* Builds a PTR query for d.c.b.a.in-addr.arpa. Returns the length, or -1. */
static int dns_build_ptr(uint8_t *buf, size_t cap, uint16_t id, uint32_t ip)
{
    /* 12 header + at most 4x4 octet labels + in-addr + arpa + root + 4 */
    if (cap < 64) {
        return -1;
    }

    size_t p = 0;
    buf[p++] = (uint8_t)(id >> 8);
    buf[p++] = (uint8_t)(id & 0xff);
    buf[p++] = 0x01;                    /* RD set                            */
    buf[p++] = 0x00;
    buf[p++] = 0x00; buf[p++] = 0x01;   /* QDCOUNT 1                         */
    buf[p++] = 0x00; buf[p++] = 0x00;   /* ANCOUNT                           */
    buf[p++] = 0x00; buf[p++] = 0x00;   /* NSCOUNT                           */
    buf[p++] = 0x00; buf[p++] = 0x00;   /* ARCOUNT                           */

    /* ip is host byte order, so the low octet is the leading PTR label. */
    for (int shift = 0; shift < 32; shift += 8) {
        char      oct[4];
        const int n = snprintf(oct, sizeof(oct), "%u",
                               (unsigned)((ip >> shift) & 0xffu));
        if (n <= 0 || n > 3) {
            return -1;
        }
        buf[p++] = (uint8_t)n;
        memcpy(buf + p, oct, (size_t)n);
        p += (size_t)n;
    }

    static const char *const suffix[] = { "in-addr", "arpa" };
    for (size_t i = 0; i < 2; i++) {
        const size_t n = strlen(suffix[i]);
        buf[p++] = (uint8_t)n;
        memcpy(buf + p, suffix[i], n);
        p += n;
    }

    buf[p++] = 0x00;                    /* root label                        */
    buf[p++] = 0x00; buf[p++] = DNS_TYPE_PTR;
    buf[p++] = 0x00; buf[p++] = DNS_CLASS_IN;
    return (int)p;
}

/* Steps over a (possibly compressed) name. Returns the next offset, or -1. */
static int dns_skip_name(const uint8_t *buf, int len, int pos)
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
            return -1;                  /* reserved label type               */
        }
        pos += 1 + l;
    }
    return -1;
}

/*
 * Decodes a name into out. Compression pointers are followed at most
 * RDNS_MAX_JUMPS times and may only point backwards, so a hand-crafted reply
 * cannot loop us.
 */
static bool dns_read_name(const uint8_t *buf, int len, int pos, char *out, size_t out_cap)
{
    size_t o     = 0;
    int    jumps = 0;
    int    limit = pos;

    while (pos >= 0 && pos < len) {
        const uint8_t l = buf[pos];
        if (l == 0) {
            break;
        }
        if ((l & 0xc0) == 0xc0) {
            if (pos + 1 >= len || ++jumps > RDNS_MAX_JUMPS) {
                return false;
            }
            const int target = ((l & 0x3f) << 8) | buf[pos + 1];
            if (target >= limit) {
                return false;           /* must point backwards              */
            }
            limit = target;
            pos   = target;
            continue;
        }
        if ((l & 0xc0) != 0 || pos + 1 + l > len) {
            return false;
        }
        if (o != 0) {
            if (o + 1 >= out_cap) {
                return false;
            }
            out[o++] = '.';
        }
        if (o + l >= out_cap) {
            return false;
        }
        memcpy(out + o, buf + pos + 1, l);
        o   += l;
        pos += 1 + l;
    }

    out[o] = '\0';
    return o > 0;
}

/* Finds the first PTR record in the answer section. */
static bool dns_first_ptr(const uint8_t *buf, int len, uint16_t id,
                          char *out, size_t out_cap)
{
    if (len < DNS_HEADER_LEN) {
        return false;
    }
    if (((buf[0] << 8) | buf[1]) != id) {
        return false;                   /* stale or spoofed reply            */
    }
    if ((buf[2] & 0x80) == 0) {
        return false;                   /* not a response                    */
    }
    if ((buf[3] & 0x0f) != 0) {
        return false;                   /* RCODE, usually NXDOMAIN           */
    }

    const int qd = (buf[4] << 8) | buf[5];
    const int an = (buf[6] << 8) | buf[7];
    if (an < 1) {
        return false;
    }

    int pos = DNS_HEADER_LEN;
    for (int i = 0; i < qd; i++) {
        pos = dns_skip_name(buf, len, pos);
        if (pos < 0 || pos + 4 > len) {
            return false;
        }
        pos += 4;                       /* QTYPE + QCLASS                    */
    }

    for (int i = 0; i < an; i++) {
        pos = dns_skip_name(buf, len, pos);
        if (pos < 0 || pos + 10 > len) {
            return false;
        }
        const int type     = (buf[pos] << 8) | buf[pos + 1];
        const int rdlength = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;

        if (pos + rdlength > len) {
            return false;
        }
        if (type == DNS_TYPE_PTR) {
            return dns_read_name(buf, len, pos, out, out_cap);
        }
        pos += rdlength;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Pass                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Keeps the first label only. "kitchen-pi.lan" and "kitchen-pi.localdomain"
 * both become "kitchen-pi"; device_db does the rest of the sanitising.
 */
static void first_label(char *name)
{
    char *dot = strchr(name, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
}

esp_err_t disc_rdns_run_once(void)
{
    const uint32_t gateway = wifi_mgr_get_gateway();
    if (gateway == 0) {
        ESP_LOGD(TAG, "no gateway, skipping");
        return ESP_OK;
    }

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
        .tv_sec  = RDNS_TIMEOUT_MS / 1000,
        .tv_usec = (RDNS_TIMEOUT_MS % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(RDNS_PORT),
    };
    dst.sin_addr.s_addr = htonl(gateway);

    int queries = 0;
    int named   = 0;

    for (size_t i = 0; i < count && queries < RDNS_MAX_QUERIES; i++) {
        /* Anything already named by mDNS or a previous rDNS pass is skipped;
           SSDP and NBNS names are lower priority, so those are re-queried. */
        if (snap[i].name_src >= NETDASH_NAME_SRC_RDNS) {
            continue;
        }

        const uint16_t id  = (uint16_t)(esp_random() & 0xffff);
        const int      len = dns_build_ptr(s_tx, sizeof(s_tx), id, snap[i].ip);
        if (len < 0) {
            continue;
        }

        queries++;
        if (sendto(sock, s_tx, (size_t)len, 0,
                   (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            ESP_LOGD(TAG, "sendto failed: errno %d", errno);
            continue;
        }

        const int n = recv(sock, s_rx, sizeof(s_rx), 0);
        if (n > 0) {
            char name[64];
            if (dns_first_ptr(s_rx, n, id, name, sizeof(name))) {
                first_label(name);
                if (name[0] != '\0') {
                    (void)device_db_set_hostname(snap[i].mac, name,
                                                 NETDASH_NAME_SRC_RDNS);
                    named++;
                    ESP_LOGD(TAG, "%u.%u.%u.%u -> %s",
                             (unsigned)((snap[i].ip >> 24) & 0xff),
                             (unsigned)((snap[i].ip >> 16) & 0xff),
                             (unsigned)((snap[i].ip >> 8) & 0xff),
                             (unsigned)(snap[i].ip & 0xff), name);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RDNS_GAP_MS));
    }

    close(sock);
    disc_snapshot_release();

    ESP_LOGI(TAG, "rdns: %d queries, %d named", queries, named);
    return ESP_OK;
}

esp_err_t disc_rdns_init(void)
{
    return disc_task_ensure();
}
