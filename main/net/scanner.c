/*
 * NetDash subnet scanner.
 *
 * One task owns everything: it waits for a STA IP, then walks the connected
 * subnet sending a single paced ICMP echo per host, resolves each host's MAC
 * out of the lwIP ARP table a short time after the echo went out, and finishes
 * with a snapshot of whatever else the ARP table learned passively.
 *
 * Design notes
 * ------------
 * - One raw ICMP socket, non-blocking, drained from the same task with
 *   select() in the gaps between sends. No second task, no per-host
 *   allocation, no per-host array.
 * - Sending the echo is what forces lwIP to ARP the target, so hosts that drop
 *   ICMP still show up (MAC found, rtt_ms == -1).
 * - A tiny ring of pending probes keeps the ARP lookup roughly SCAN_ARP_LAG_MS
 *   behind the sends and never more than PENDING_RING hosts behind, so the
 *   10-entry lwIP ARP table cannot evict an entry before it is read.
 * - Every etharp_* call runs inside esp_netif_tcpip_exec().
 */
#include "scanner.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"

#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"

#include "app_events.h"
#include "device_db.h"
#include "notify.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "scanner";

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

#define SCAN_TASK_STACK      5120
#define SCAN_TASK_PRIO       4

/* 'N' 'D' - identifies our echoes in the reply stream. */
#define SCAN_ICMP_ID         0x4E44u
#define SCAN_ICMP_PAYLOAD    8
#define SCAN_ICMP_LEN        (8 + SCAN_ICMP_PAYLOAD)

/* Shortest prefix we are willing to walk (/22 == 1022 hosts). */
#define SCAN_MIN_PREFIX      22

/* Settle time between "got IP" and the first sweep. */
#define SCAN_SETTLE_MS       10000

/* How far the ARP lookup lags the echo, and how many probes may be in flight. */
#define SCAN_ARP_LAG_MS      300
#define PENDING_RING         8

/* SCAN_PROGRESS event cadence. */
#define SCAN_PROGRESS_EVERY  16

/*
 * How often the whole ARP table is harvested during the walk. Eight probes is
 * two seconds at the default rate, comfortably inside the lifetime of a stable
 * entry now that ARP_TABLE_SIZE is raised (see the top-level CMakeLists.txt).
 */
#define SCAN_ARP_HARVEST_EVERY 8

/* Consecutive transient sendto() failures that abort a sweep. */
#define SCAN_MAX_SEND_ERRORS 16

#define ICMP_ECHO_REQUEST    8
#define ICMP_ECHO_REPLY      0

/* icmp_send() results. */
#define SEND_OK              1
#define SEND_TRANSIENT       0
#define SEND_FATAL           (-1)

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_wake;              /* trigger / got-IP doorbell   */
static volatile bool     s_have_ip;
static volatile bool     s_settle_pending;
static volatile bool     s_running;
static volatile uint16_t s_progress_done;
static volatile uint16_t s_progress_total;
static int64_t           s_last_sweep;        /* guarded by s_mux            */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * MACs already recorded during the running sweep. Only the closing ARP-table
 * snapshot reads it, so that a passively learned entry cannot overwrite the
 * rtt_ms of a host that actually answered its echo.
 */
static uint8_t  s_seen_macs[NETDASH_MAX_DEVICES][6];
static uint16_t s_seen_count;

/* Per-sweep state, kept out of the stack and off the ring helpers' arguments. */
typedef struct {
    struct netif *netif;
    int64_t       now_unix;
    uint16_t      probed;
    uint16_t      alive;        /* hosts resolved during the active walk     */
    uint16_t      replies;      /* of those, how many answered ICMP          */
    uint16_t      fresh;        /* devices new to the database               */
    uint16_t      from_arp;     /* extra hosts found in the closing snapshot */
    bool          aborted;
} sweep_ctx_t;

static sweep_ctx_t s_sweep;

/* One in-flight probe. */
typedef struct {
    uint32_t ip;          /* host byte order                                 */
    uint16_t seq;
    int64_t  sent_us;
    int16_t  rtt_ms;      /* -1 until a matching reply arrives               */
} pending_t;

static pending_t s_ring[PENDING_RING];
static uint8_t   s_ring_head;   /* next write slot */
static uint8_t   s_ring_tail;   /* oldest entry    */
static uint8_t   s_ring_used;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* device_db's convention: unix seconds once the clock is trustworthy, else 0. */
static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return (t > (time_t)1600000000) ? (int64_t)t : 0;
}

static void set_last_sweep(int64_t ts)
{
    taskENTER_CRITICAL(&s_mux);
    s_last_sweep = ts;
    taskEXIT_CRITICAL(&s_mux);
}

static void post_scan_event(int32_t id, uint16_t done, uint16_t total)
{
    netdash_scan_progress_t ev = { .done = done, .total = total };
    esp_event_post(NETDASH_EVENT, id, &ev, sizeof(ev), 0);
}

static uint8_t prefix_len(uint32_t mask)
{
    uint8_t bits = 0;
    while (mask & 0x80000000u) {
        bits++;
        mask <<= 1;
    }
    return bits;
}

static void mac_seen_reset(void)
{
    s_seen_count = 0;
}

static void mac_seen_add(const uint8_t mac[6])
{
    if (s_seen_count < NETDASH_MAX_DEVICES) {
        memcpy(s_seen_macs[s_seen_count++], mac, 6);
    }
}

static bool mac_seen(const uint8_t mac[6])
{
    for (uint16_t i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen_macs[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

/* Standard internet checksum over a buffer already in network byte order. */
static uint16_t icmp_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len > 0) {
        sum += (uint32_t)data[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

/* ------------------------------------------------------------------------- */
/* lwIP calls, all marshalled into the TCP/IP thread                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    struct netif *netif;
    ip4_addr_t    ip;
    uint8_t       mac[6];
    bool          found;
} arp_find_ctx_t;

static esp_err_t arp_find_cb(void *ctx)
{
    arp_find_ctx_t   *c     = (arp_find_ctx_t *)ctx;
    struct eth_addr  *eth   = NULL;
    const ip4_addr_t *ipret = NULL;

    if (c->netif != NULL &&
        etharp_find_addr(c->netif, &c->ip, &eth, &ipret) >= 0 && eth != NULL) {
        memcpy(c->mac, eth->addr, 6);
        c->found = true;
    }
    return ESP_OK;
}

/* ip is host byte order. Returns true and fills mac when the ARP entry exists. */
static bool arp_lookup(struct netif *nif, uint32_t ip, uint8_t mac[6])
{
    static arp_find_ctx_t ctx;   /* one scanner task only; keeps the stack small */

    memset(&ctx, 0, sizeof(ctx));
    ctx.netif   = nif;
    ctx.ip.addr = lwip_htonl(ip);

    if (esp_netif_tcpip_exec(arp_find_cb, &ctx) != ESP_OK || !ctx.found) {
        return false;
    }
    memcpy(mac, ctx.mac, 6);
    return true;
}

typedef struct {
    struct netif *netif;
    uint8_t       count;
    struct {
        uint32_t ip;          /* host byte order */
        uint8_t  mac[6];
    } entry[ARP_TABLE_SIZE];
} arp_dump_ctx_t;

static esp_err_t arp_dump_cb(void *ctx)
{
    arp_dump_ctx_t *c = (arp_dump_ctx_t *)ctx;

    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t      *ip  = NULL;
        struct netif    *nif = NULL;
        struct eth_addr *eth = NULL;

        if (etharp_get_entry(i, &ip, &nif, &eth) == 0) {
            continue;   /* empty or pending slot */
        }
        if (ip == NULL || eth == NULL || nif != c->netif) {
            continue;
        }
        if (c->count < ARP_TABLE_SIZE) {
            c->entry[c->count].ip = lwip_ntohl(ip->addr);
            memcpy(c->entry[c->count].mac, eth->addr, 6);
            c->count++;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Pending probe ring                                                        */
/* ------------------------------------------------------------------------- */

static void ring_reset(void)
{
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_used = 0;
}

static void log_host(const char *what, uint32_t ip)
{
    ESP_LOGD(TAG, "%s %u.%u.%u.%u", what,
             (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
}

/* Resolves the oldest pending probe and hands it to the device database. */
static void ring_pop_resolve(void)
{
    if (s_ring_used == 0) {
        return;
    }

    pending_t entry = s_ring[s_ring_tail];
    s_ring_tail = (uint8_t)((s_ring_tail + 1) % PENDING_RING);
    s_ring_used--;

    uint8_t mac[6];
    if (!arp_lookup(s_sweep.netif, entry.ip, mac)) {
        log_host("no ARP entry for", entry.ip);
        return;
    }

    /*
     * The periodic table harvest may already have recorded this host. Upsert
     * anyway, because only this path knows the round-trip time, but do not
     * count it twice.
     */
    const bool already = mac_seen(mac);

    /* No database lock is held here; upsert_seen takes its own. */
    if (device_db_upsert_seen(mac, entry.ip, entry.rtt_ms, s_sweep.now_unix)) {
        s_sweep.fresh++;
    }
    if (!already) {
        mac_seen_add(mac);
        s_sweep.alive++;
    }

    ESP_LOGD(TAG, "%u.%u.%u.%u is %02x:%02x:%02x:%02x:%02x:%02x rtt=%d ms",
             (unsigned)(entry.ip >> 24), (unsigned)((entry.ip >> 16) & 0xff),
             (unsigned)((entry.ip >> 8) & 0xff), (unsigned)(entry.ip & 0xff),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)entry.rtt_ms);
}

static void ring_push(uint32_t ip, uint16_t seq, int64_t sent_us)
{
    if (s_ring_used == PENDING_RING) {
        ring_pop_resolve();     /* never lag further than the ARP window */
    }
    s_ring[s_ring_head].ip      = ip;
    s_ring[s_ring_head].seq     = seq;
    s_ring[s_ring_head].sent_us = sent_us;
    s_ring[s_ring_head].rtt_ms  = -1;
    s_ring_head = (uint8_t)((s_ring_head + 1) % PENDING_RING);
    s_ring_used++;
}

static void ring_flush_due(int64_t now_us)
{
    while (s_ring_used > 0 &&
           now_us - s_ring[s_ring_tail].sent_us >= (int64_t)SCAN_ARP_LAG_MS * 1000) {
        ring_pop_resolve();
    }
}

static void ring_flush_all(void)
{
    while (s_ring_used > 0) {
        ring_pop_resolve();
    }
}

/* Absolute time the oldest entry falls due, or 0 when the ring is empty. */
static int64_t ring_next_due(void)
{
    if (s_ring_used == 0) {
        return 0;
    }
    return s_ring[s_ring_tail].sent_us + (int64_t)SCAN_ARP_LAG_MS * 1000;
}

static void ring_record_reply(uint32_t src_ip, uint16_t seq, int64_t now_us)
{
    for (uint8_t i = 0; i < s_ring_used; i++) {
        uint8_t idx = (uint8_t)((s_ring_tail + i) % PENDING_RING);

        if (s_ring[idx].seq == seq && s_ring[idx].ip == src_ip) {
            if (s_ring[idx].rtt_ms < 0) {
                int64_t rtt = (now_us - s_ring[idx].sent_us) / 1000;
                if (rtt < 0) {
                    rtt = 0;
                } else if (rtt > 32767) {
                    rtt = 32767;
                }
                s_ring[idx].rtt_ms = (int16_t)rtt;
                s_sweep.replies++;
            }
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Socket work                                                               */
/* ------------------------------------------------------------------------- */

static int icmp_socket_open(void)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGE(TAG, "raw ICMP socket failed (errno %d)", errno);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "setting O_NONBLOCK failed (errno %d)", errno);
        close(sock);
        return -1;
    }
    return sock;
}

static int icmp_send(int sock, uint32_t ip, uint16_t seq, int64_t sent_us)
{
    uint8_t pkt[SCAN_ICMP_LEN];

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = ICMP_ECHO_REQUEST;
    pkt[1] = 0;
    /* pkt[2..3] is the checksum, filled in below. */
    pkt[4] = (uint8_t)(SCAN_ICMP_ID >> 8);
    pkt[5] = (uint8_t)(SCAN_ICMP_ID & 0xff);
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)(seq & 0xff);
    for (int i = 0; i < SCAN_ICMP_PAYLOAD; i++) {
        pkt[8 + i] = (uint8_t)(sent_us >> (8 * (7 - i)));
    }

    uint16_t sum = icmp_checksum(pkt, sizeof(pkt));
    pkt[2] = (uint8_t)(sum >> 8);
    pkt[3] = (uint8_t)(sum & 0xff);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = lwip_htonl(ip);

    int sent = sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
    if (sent >= 0) {
        return SEND_OK;
    }
    if (errno == EBADF || errno == ENOTSOCK || errno == EAFNOSUPPORT ||
        errno == ENETUNREACH) {
        ESP_LOGE(TAG, "sendto failed fatally (errno %d)", errno);
        return SEND_FATAL;
    }
    ESP_LOGD(TAG, "sendto %u.%u.%u.%u failed (errno %d)",
             (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff), errno);
    return SEND_TRANSIENT;
}

/* Reads every reply already queued. Returns false on a fatal socket error. */
static bool icmp_drain(int sock)
{
    for (;;) {
        uint8_t            buf[96];
        struct sockaddr_in from;
        socklen_t          from_len = sizeof(from);

        int n = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                return true;
            }
            ESP_LOGE(TAG, "recvfrom failed (errno %d)", errno);
            return false;
        }
        if (n == 0) {
            return true;
        }

        /* A raw IPv4 socket hands us the IP header as well. */
        if (n < 20) {
            continue;
        }
        size_t ihl = (size_t)(buf[0] & 0x0f) * 4;
        if (ihl < 20 || (size_t)n < ihl + 8) {
            continue;
        }
        const uint8_t *icmp = buf + ihl;
        if (icmp[0] != ICMP_ECHO_REPLY) {
            continue;
        }
        uint16_t id = (uint16_t)(((uint16_t)icmp[4] << 8) | icmp[5]);
        if (id != SCAN_ICMP_ID) {
            continue;   /* somebody else's ping */
        }
        uint16_t seq = (uint16_t)(((uint16_t)icmp[6] << 8) | icmp[7]);
        uint32_t src = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16) |
                       ((uint32_t)buf[14] << 8) | (uint32_t)buf[15];

        ring_record_reply(src, seq, esp_timer_get_time());
    }
}

/*
 * Waits until deadline_us, draining replies and resolving due ARP lookups on
 * the way. Returns false when the sweep must be abandoned.
 */
static bool pace_until(int sock, int64_t deadline_us)
{
    for (;;) {
        int64_t now  = esp_timer_get_time();
        int64_t wake = deadline_us - now;

        if (wake <= 0) {
            ring_flush_due(now);
            return true;
        }

        int64_t due = ring_next_due();
        if (due != 0 && (due - now) < wake) {
            wake = due - now;
        }
        if (wake < 1000) {
            wake = 1000;            /* never spin on a zero timeout */
        } else if (wake > 100000) {
            wake = 100000;          /* stay responsive to Wi-Fi loss */
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {
            .tv_sec  = (time_t)(wake / 1000000),
            .tv_usec = (suseconds_t)(wake % 1000000),
        };

        int r = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "select failed (errno %d)", errno);
            return false;
        }
        if (r > 0 && !icmp_drain(sock)) {
            return false;
        }

        ring_flush_due(esp_timer_get_time());

        if (!s_have_ip) {
            ESP_LOGW(TAG, "Wi-Fi lost mid-sweep");
            return false;
        }
        if (esp_timer_get_time() >= deadline_us) {
            return true;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Closing ARP-table snapshot                                                */
/* ------------------------------------------------------------------------- */

static void arp_snapshot(uint32_t network, uint32_t mask)
{
    static arp_dump_ctx_t dump;   /* ~110 bytes, deliberately off the stack */

    memset(&dump, 0, sizeof(dump));
    dump.netif = s_sweep.netif;

    if (esp_netif_tcpip_exec(arp_dump_cb, &dump) != ESP_OK) {
        ESP_LOGW(TAG, "ARP table snapshot failed");
        return;
    }

    for (uint8_t i = 0; i < dump.count; i++) {
        uint32_t ip = dump.entry[i].ip;

        if (mask != 0 && (ip & mask) != network) {
            continue;   /* off-subnet leftovers */
        }
        if (mac_seen(dump.entry[i].mac)) {
            continue;
        }
        if (device_db_upsert_seen(dump.entry[i].mac, ip, -1, s_sweep.now_unix)) {
            s_sweep.fresh++;
        }
        mac_seen_add(dump.entry[i].mac);
        s_sweep.from_arp++;
        log_host("arp-only", ip);
    }
}

/* ------------------------------------------------------------------------- */
/* One sweep                                                                 */
/* ------------------------------------------------------------------------- */

static struct netif *sta_netif_impl(void)
{
    /* wifi_mgr.h exposes no esp_netif handle, so go through the standard key. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif == NULL) {
        return NULL;
    }
    return (struct netif *)esp_netif_get_netif_impl(netif);
}

static void sweep_finish_idle(void)
{
    s_running        = false;
    s_progress_done  = 0;
    s_progress_total = 0;
}

static void sweep(const netdash_settings_t *cfg)
{
    esp_netif_ip_info_t info;

    /*
     * get_ip_info() falls back to the softAP netif, and get_gateway() is only
     * non-zero once the STA has an IP, so this pair means "really on the LAN".
     */
    if (wifi_mgr_get_ip_info(&info) != ESP_OK || wifi_mgr_get_gateway() == 0) {
        ESP_LOGW(TAG, "no STA IP, skipping sweep");
        post_scan_event(NETDASH_EVENT_SCAN_DONE, 0, 0);
        return;
    }

    uint32_t self      = lwip_ntohl(info.ip.addr);
    uint32_t mask      = lwip_ntohl(info.netmask.addr);
    uint32_t gw        = lwip_ntohl(info.gw.addr);
    uint8_t  prefix    = prefix_len(mask);
    uint32_t network   = self & mask;
    uint32_t broadcast = network | ~mask;

    struct netif *nif = sta_netif_impl();
    if (nif == NULL) {
        ESP_LOGE(TAG, "STA netif unavailable, skipping sweep");
        post_scan_event(NETDASH_EVENT_SCAN_DONE, 0, 0);
        return;
    }

    memset(&s_sweep, 0, sizeof(s_sweep));
    s_sweep.netif    = nif;
    s_sweep.now_unix = now_unix();
    ring_reset();
    mac_seen_reset();

    bool     active = !cfg->passive_only;
    uint16_t total  = 0;

    if (active) {
        if (prefix < SCAN_MIN_PREFIX) {
            ESP_LOGW(TAG, "subnet /%u is too large to sweep politely (minimum /%u)",
                     (unsigned)prefix, (unsigned)SCAN_MIN_PREFIX);
            post_scan_event(NETDASH_EVENT_SCAN_DONE, 0, 0);
            return;
        }
        if (prefix > 30) {
            ESP_LOGW(TAG, "subnet /%u has no probeable hosts", (unsigned)prefix);
            post_scan_event(NETDASH_EVENT_SCAN_DONE, 0, 0);
            return;
        }
        /* Every host except the network address, the broadcast and ourselves. */
        total = (uint16_t)((1u << (32 - prefix)) - 3);
    }

    uint16_t hosts_per_sec = (cfg->hosts_per_sec > 0) ? cfg->hosts_per_sec : 4;
    int64_t  slot_us       = 1000000 / (int64_t)hosts_per_sec;
    int64_t  t0            = esp_timer_get_time();

    s_running        = true;
    s_progress_done  = 0;
    s_progress_total = total;
    post_scan_event(NETDASH_EVENT_SCAN_STARTED, 0, total);

    if (active) {
        ESP_LOGI(TAG, "sweeping %u.%u.%u.%u/%u (%u hosts at %u/s), gw %u.%u.%u.%u",
                 (unsigned)(network >> 24), (unsigned)((network >> 16) & 0xff),
                 (unsigned)((network >> 8) & 0xff), (unsigned)(network & 0xff),
                 (unsigned)prefix, (unsigned)total, (unsigned)hosts_per_sec,
                 (unsigned)(gw >> 24), (unsigned)((gw >> 16) & 0xff),
                 (unsigned)((gw >> 8) & 0xff), (unsigned)(gw & 0xff));

        int sock = icmp_socket_open();
        if (sock < 0) {
            sweep_finish_idle();
            post_scan_event(NETDASH_EVENT_SCAN_DONE, 0, 0);
            return;     /* aborted: device_db_mark_sweep_end() is NOT called */
        }

        uint16_t seq         = 0;
        uint16_t send_errors = 0;
        int64_t  slot_end    = esp_timer_get_time();

        for (uint32_t ip = network + 1; ip < broadcast; ip++) {
            if (ip == self) {
                continue;   /* the gateway is deliberately not skipped */
            }
            if (!s_have_ip) {
                ESP_LOGW(TAG, "Wi-Fi lost mid-sweep");
                s_sweep.aborted = true;
                break;
            }

            int64_t sent_us = esp_timer_get_time();
            int     rc      = icmp_send(sock, ip, seq, sent_us);
            if (rc == SEND_FATAL) {
                s_sweep.aborted = true;
                break;
            }
            if (rc == SEND_TRANSIENT) {
                if (++send_errors >= SCAN_MAX_SEND_ERRORS) {
                    ESP_LOGE(TAG, "too many send errors, abandoning sweep");
                    s_sweep.aborted = true;
                    break;
                }
            } else {
                send_errors = 0;
                ring_push(ip, seq, sent_us);
            }
            seq++;
            s_sweep.probed++;
            s_progress_done = s_sweep.probed;

            if ((s_sweep.probed % SCAN_PROGRESS_EVERY) == 0) {
                post_scan_event(NETDASH_EVENT_SCAN_PROGRESS, s_sweep.probed, total);
            }

            /*
             * Harvest the whole ARP table as we go, not only at the end.
             *
             * lwIP recycles the oldest *stable* entry before it touches a
             * pending one, so every probe to an address that does not answer
             * can evict a host we already resolved. Sampling the table often
             * catches those entries while they are still there; the closing
             * snapshot alone used to lose most of them.
             */
            if ((s_sweep.probed % SCAN_ARP_HARVEST_EVERY) == 0) {
                arp_snapshot(network, mask);
            }

            /* Hold the pace even when a send or a lookup took a while. */
            slot_end += slot_us;
            if (slot_end < sent_us) {
                slot_end = sent_us + slot_us;
            }
            if (!pace_until(sock, slot_end)) {
                s_sweep.aborted = true;
                break;
            }
        }

        if (!s_sweep.aborted) {
            /* Let the last probes age past the ARP lag before resolving them. */
            (void)pace_until(sock, esp_timer_get_time() + (int64_t)SCAN_ARP_LAG_MS * 1000);
        }
        ring_flush_all();
        close(sock);
    }

    if (s_sweep.aborted) {
        ESP_LOGW(TAG, "sweep aborted after %u hosts", (unsigned)s_sweep.probed);
        sweep_finish_idle();
        post_scan_event(NETDASH_EVENT_SCAN_DONE, s_sweep.probed, total);
        return;     /* aborted: device_db_mark_sweep_end() is NOT called */
    }

    arp_snapshot(network, mask);

    /* A sweep can outlast the first NTP sync, so re-read the clock at the end. */
    int64_t end_unix = now_unix();
    if (end_unix != 0) {
        s_sweep.now_unix = end_unix;
    }
    /*
     * Only an active walk is evidence of absence. In passive_only mode the
     * 10-entry ARP table is all we have, so ageing miss_count here would take
     * nearly every device offline after three intervals; the online state is
     * frozen instead and the snapshot above can still bring devices back.
     */
    if (active) {
        device_db_mark_sweep_end(s_sweep.now_unix);
    }
    set_last_sweep(end_unix);

    /*
     * Arm the notification feed only once a full sweep has been and gone.
     * Everything found during that first sweep is a device we simply did not
     * know about yet - on a freshly flashed dongle that is the entire network,
     * and delivering it as twenty-odd "new device" alerts would teach the user
     * to ignore the feed on day one.
     */
    notify_arm();

    uint32_t dur_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    uint16_t alive  = (uint16_t)(s_sweep.alive + s_sweep.from_arp);

    s_progress_done = s_sweep.probed;
    post_scan_event(NETDASH_EVENT_SCAN_PROGRESS, s_sweep.probed, total);
    post_scan_event(NETDASH_EVENT_SCAN_DONE, s_sweep.probed, total);

    ESP_LOGI(TAG, "%s sweep: %u probed, %u alive (%u icmp, %u arp-only), %u new, %u.%03u s",
             active ? "active" : "passive", (unsigned)s_sweep.probed, (unsigned)alive,
             (unsigned)s_sweep.replies, (unsigned)s_sweep.from_arp,
             (unsigned)s_sweep.fresh, (unsigned)(dur_ms / 1000), (unsigned)(dur_ms % 1000));

    /*
     * Only log a sweep that actually tells the user something.
     *
     * A sweep every couple of minutes, each writing "22 alive / 253 probed",
     * fills the 100-entry ring in a few hours and pushes out the entries that
     * matter - a device appearing, or going offline. The serial log above
     * still records every sweep, and /api/status carries the live counts and
     * the last sweep time, so nothing is lost by staying quiet here.
     */
    static uint16_t s_last_logged_alive;
    static bool     s_logged_once;

    if (!s_logged_once || alive != s_last_logged_alive) {
        char line[48];
        snprintf(line, sizeof(line), "%u device%s on the network",
                 (unsigned)alive, alive == 1 ? "" : "s");
        events_log_push(NETDASH_LOG_SCAN, NULL, 0, line);
        s_last_logged_alive = alive;
        s_logged_once       = true;
    }

    sweep_finish_idle();
}

/* ------------------------------------------------------------------------- */
/* Task                                                                      */
/* ------------------------------------------------------------------------- */

/* Sleeps in chunks so that losing the IP cuts the wait short. */
static bool settle_delay(uint32_t ms)
{
    while (ms > 0 && s_have_ip) {
        uint32_t chunk = (ms > 200) ? 200 : ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
    return s_have_ip;
}

/* Returns true when a manual trigger woke us before the interval elapsed. */
static bool wait_interval(uint16_t minutes)
{
    int64_t end = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000;

    while (s_have_ip) {
        int64_t remain_ms = (end - esp_timer_get_time()) / 1000;
        if (remain_ms <= 0) {
            return false;
        }
        if (remain_ms > 1000) {
            remain_ms = 1000;   /* chunked so Wi-Fi loss is noticed promptly */
        }
        if (xSemaphoreTake(s_wake, pdMS_TO_TICKS((uint32_t)remain_ms)) == pdTRUE) {
            return true;
        }
    }
    return false;
}

static void scanner_task(void *arg)
{
    (void)arg;

    /* The IP may already have arrived before this task existed. */
    if (wifi_mgr_get_gateway() != 0) {
        s_have_ip        = true;
        s_settle_pending = true;
    }

    for (;;) {
        while (!s_have_ip) {
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(500));
        }

        if (s_settle_pending) {
            s_settle_pending = false;
            if (!settle_delay(SCAN_SETTLE_MS)) {
                continue;
            }
        }

        /* Re-read every cycle so PUT /api/settings applies without a reboot. */
        netdash_settings_t cfg;
        settings_get(&cfg);

        sweep(&cfg);

        /* A trigger raised during the sweep is deliberately ignored. */
        xSemaphoreTake(s_wake, 0);

        if (!s_have_ip) {
            continue;
        }
        wait_interval((cfg.scan_interval_min > 0) ? cfg.scan_interval_min : 5);
    }
}

/* ------------------------------------------------------------------------- */
/* Events                                                                    */
/* ------------------------------------------------------------------------- */

static void netdash_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (id) {
    case NETDASH_EVENT_WIFI_STA_GOT_IP:
        s_have_ip        = true;
        s_settle_pending = true;
        if (s_wake != NULL) {
            xSemaphoreGive(s_wake);
        }
        break;
    case NETDASH_EVENT_WIFI_STA_LOST:
        s_have_ip = false;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t scanner_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_wake = xSemaphoreCreateBinary();
    if (s_wake == NULL) {
        ESP_LOGE(TAG, "semaphore allocation failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(
        NETDASH_EVENT, NETDASH_EVENT_WIFI_STA_GOT_IP, netdash_event_handler, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            NETDASH_EVENT, NETDASH_EVENT_WIFI_STA_LOST, netdash_event_handler, NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event registration failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_wake);
        s_wake = NULL;
        return err;
    }

    if (xTaskCreate(scanner_task, "netdash_scan", SCAN_TASK_STACK, NULL,
                    SCAN_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        vSemaphoreDelete(s_wake);
        s_wake = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "scanner task started");
    return ESP_OK;
}

void scanner_trigger_now(void)
{
    if (s_wake == NULL || s_running) {
        return;     /* ignored while a sweep is running */
    }
    xSemaphoreGive(s_wake);
}

bool scanner_is_running(void)
{
    return s_running;
}

int64_t scanner_last_sweep_time(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t ts = s_last_sweep;
    taskEXIT_CRITICAL(&s_mux);
    return ts;
}

void scanner_get_progress(uint16_t *done, uint16_t *total)
{
    bool running = s_running;

    if (done != NULL) {
        *done = running ? s_progress_done : 0;
    }
    if (total != NULL) {
        *total = running ? s_progress_total : 0;
    }
}
