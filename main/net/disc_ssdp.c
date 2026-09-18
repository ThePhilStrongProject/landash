/*
 * SSDP discovery.
 *
 * One M-SEARCH burst per pass collects every UPnP responder on the link. That
 * alone is worth having: an SSDP reply proves the device runs a UPnP stack,
 * which classify.c uses to tell an ASUS router apart from a dumb switch.
 *
 * Responders that device_db still has no name for get their description XML
 * fetched, which is where the useful names live: LG webOS TVs, DLNA servers on
 * a NAS and ASUS routers all publish a friendlyName. The fetch is capped hard
 * (count, size and timeout) because it is the only part of discovery that
 * makes a TCP connection.
 */
#include "disc_ssdp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "device_db.h"
#include "disc_mdns.h"

static const char *TAG = "disc_ssdp";

#define SSDP_ADDR             "239.255.255.250"
#define SSDP_PORT             1900
#define SSDP_BURSTS           2
#define SSDP_BURST_GAP_MS     500
#define SSDP_LISTEN_MS        3000
#define SSDP_POLL_MS          200

#define SSDP_MAX_RESPONDERS   16
#define SSDP_RX_CAP           1024

#define SSDP_MAX_FETCHES      6
#define SSDP_FETCH_TIMEOUT_MS 2000
#define SSDP_XML_CAP          4096

/* MX must match the MX header below; responders spread replies over it. */
static const char SSDP_MSEARCH[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: " SSDP_ADDR ":1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "\r\n";

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;                    /* host byte order                      */
    bool     want_name;             /* device_db has no name for it at all  */
    char     location[160];         /* description URL, empty when absent   */
} ssdp_responder_t;

/*
 * Static because they are large and the four discovery sources are serialised
 * by the snapshot mutex, so only one of them is ever using its buffers.
 */
static ssdp_responder_t s_resp[SSDP_MAX_RESPONDERS];
static char             s_rx[SSDP_RX_CAP];
static char             s_xml[SSDP_XML_CAP];

/* ------------------------------------------------------------------------- */
/* Small parsers                                                             */
/* ------------------------------------------------------------------------- */

/* Case-insensitive substring search; strcasestr is not portable enough here. */
static const char *ci_strstr(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0) {
        return hay;
    }
    for (const char *p = hay; *p != '\0'; p++) {
        if (strncasecmp(p, needle, n) == 0) {
            return p;
        }
    }
    return NULL;
}

/*
 * Copies one header value out of an SSDP reply. name must include the colon,
 * for example "LOCATION:". Returns false when the header is absent or empty.
 */
static bool header_value(const char *msg, const char *name, char *out, size_t out_cap)
{
    const size_t name_len = strlen(name);

    for (const char *line = msg; *line != '\0'; ) {
        const char *eol = strstr(line, "\r\n");
        if (eol == NULL || eol == line) {
            break;                              /* end of headers            */
        }
        if (strncasecmp(line, name, name_len) == 0) {
            const char *v = line + name_len;
            while (v < eol && (*v == ' ' || *v == '\t')) {
                v++;
            }
            size_t n = (size_t)(eol - v);
            if (n == 0) {
                return false;
            }
            if (n >= out_cap) {
                n = out_cap - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        line = eol + 2;
    }
    return false;
}

/*
 * Pulls friendlyName out of a UPnP device description. A real XML parser is
 * not worth 4 KB of stack here: the element is mandatory, never attributed and
 * always plain text.
 */
static bool xml_friendly_name(const char *xml, char *out, size_t out_cap)
{
    const char *open = ci_strstr(xml, "<friendlyName>");
    if (open == NULL) {
        return false;
    }
    open += strlen("<friendlyName>");

    const char *close = ci_strstr(open, "</friendlyName>");
    if (close == NULL || close <= open) {
        return false;
    }

    size_t n = (size_t)(close - open);
    if (n >= out_cap) {
        n = out_cap - 1;
    }

    /* Trim, and drop anything non-printable so device_db never stores control
       characters that the dashboard would have to escape. */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)open[i];
        if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        }
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';

    size_t lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        memmove(out, out + lead, o - lead + 1);
    }
    return out[0] != '\0';
}

/* ------------------------------------------------------------------------- */
/* Description fetch                                                         */
/* ------------------------------------------------------------------------- */

static bool fetch_friendly_name(const char *url, char *out, size_t out_cap)
{
    const esp_http_client_config_t cfg = {
        .url           = url,
        .timeout_ms    = SSDP_FETCH_TIMEOUT_MS,
        .method        = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return false;
    }

    bool ok = false;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        (void)esp_http_client_fetch_headers(cli);

        if (esp_http_client_get_status_code(cli) == 200) {
            int total = 0;
            while (total < (int)SSDP_XML_CAP - 1) {
                const int r = esp_http_client_read(cli, s_xml + total,
                                                   (int)SSDP_XML_CAP - 1 - total);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            s_xml[total < 0 ? 0 : total] = '\0';
            if (total > 0) {
                ok = xml_friendly_name(s_xml, out, out_cap);
            }
        }
        esp_http_client_close(cli);
    }

    esp_http_client_cleanup(cli);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Pass                                                                      */
/* ------------------------------------------------------------------------- */

static int responder_index(uint32_t ip, int count)
{
    for (int i = 0; i < count; i++) {
        if (s_resp[i].ip == ip) {
            return i;
        }
    }
    return -1;
}

esp_err_t disc_ssdp_run_once(void)
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
        .tv_sec  = SSDP_POLL_MS / 1000,
        .tv_usec = (SSDP_POLL_MS % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(SSDP_PORT),
    };
    dst.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    int responders = 0;
    int answers    = 0;
    int bursts     = 0;

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SSDP_LISTEN_MS);
    TickType_t       next_burst = xTaskGetTickCount();

    while (xTaskGetTickCount() < deadline) {
        if (bursts < SSDP_BURSTS && xTaskGetTickCount() >= next_burst) {
            const int sent = sendto(sock, SSDP_MSEARCH, sizeof(SSDP_MSEARCH) - 1, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
            if (sent < 0) {
                ESP_LOGW(TAG, "sendto failed: errno %d", errno);
                break;
            }
            bursts++;
            next_burst = xTaskGetTickCount() + pdMS_TO_TICKS(SSDP_BURST_GAP_MS);
        }

        struct sockaddr_in src;
        socklen_t          src_len = sizeof(src);
        const int          n       = recvfrom(sock, s_rx, sizeof(s_rx) - 1, 0,
                                              (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            continue;                           /* timeout, keep waiting     */
        }
        s_rx[n] = '\0';
        answers++;

        const uint32_t ip = ntohl(src.sin_addr.s_addr);
        const disc_snap_t *dev = disc_snapshot_find_ip(snap, count, ip);
        if (dev == NULL) {
            ESP_LOGD(TAG, "no device for %s", inet_ntoa(src.sin_addr));
            continue;
        }

        /* An SSDP reply is itself evidence, independent of the description. */
        (void)device_db_set_services(dev->mac, NETDASH_SVC_SSDP);

        int idx = responder_index(ip, responders);
        if (idx < 0) {
            if (responders >= SSDP_MAX_RESPONDERS) {
                continue;
            }
            idx = responders++;
            memcpy(s_resp[idx].mac, dev->mac, sizeof(s_resp[idx].mac));
            s_resp[idx].ip          = ip;
            s_resp[idx].want_name   = (dev->name_src == NETDASH_NAME_SRC_NONE);
            s_resp[idx].location[0] = '\0';
        }

        if (s_resp[idx].want_name && s_resp[idx].location[0] == '\0') {
            (void)header_value(s_rx, "LOCATION:", s_resp[idx].location,
                               sizeof(s_resp[idx].location));
        }
    }

    close(sock);
    disc_snapshot_release();

    /* Fetches happen with no lock held and no socket open. */
    int fetched = 0;
    int named   = 0;
    for (int i = 0; i < responders && fetched < SSDP_MAX_FETCHES; i++) {
        if (!s_resp[i].want_name || s_resp[i].location[0] == '\0') {
            continue;
        }
        fetched++;

        char name[48];
        if (fetch_friendly_name(s_resp[i].location, name, sizeof(name))) {
            (void)device_db_set_hostname(s_resp[i].mac, name, NETDASH_NAME_SRC_SSDP);
            named++;
            ESP_LOGD(TAG, "%s -> %s", s_resp[i].location, name);
        } else {
            ESP_LOGD(TAG, "%s: no friendlyName", s_resp[i].location);
        }
    }

    ESP_LOGI(TAG, "ssdp: %d replies, %d responders, %d fetched, %d named",
             answers, responders, fetched, named);
    return ESP_OK;
}

esp_err_t disc_ssdp_init(void)
{
    return disc_task_ensure();
}
