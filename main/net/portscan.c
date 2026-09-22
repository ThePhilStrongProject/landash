/*
 * Background TCP port scanner. See portscan.h for the tier model.
 *
 * Technique: non-blocking connect() on a small batch of sockets, then one
 * select() for the whole batch. A connect that completes means the port is
 * open; ECONNREFUSED means closed and arrives immediately; silence until the
 * timeout means filtered. Batching keeps throughput reasonable at a low probe
 * rate, because the timeout, not the pacing, is what costs time.
 *
 * Socket budget: lwIP has CONFIG_LWIP_MAX_SOCKETS in total and the web server
 * reserves most of them, so PS_BATCH is deliberately small. Running out is not
 * fatal - a failed socket() just ends the batch early and the ports are
 * retried on the next pass.
 */
#include "portscan.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/sockets.h"

#include "app_events.h"
#include "device_db.h"
#include "notify.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "portscan";

/* Concurrent connects per batch. Bounded by the lwIP socket budget. */
#define PS_BATCH            6

/* How long a probe waits before calling the port filtered. A LAN round trip
 * is single-digit milliseconds, so this is generous. */
#define PS_TIMEOUT_MS       700

/* Idle poll when there is nothing to do (no Wi-Fi, disabled, no devices). */
#define PS_IDLE_MS          5000

/* Wait before the first probe after boot, so discovery settles first. */
#define PS_START_DELAY_MS   30000

#define PS_TIER_COMMON      1
#define PS_TIER_WELLKNOWN   2
#define PS_TIER_FULL        3

/*
 * Tier 1. Chosen for what a home network actually runs: admin interfaces,
 * file shares, media servers, printers and the usual IoT suspects.
 */
static const uint16_t s_common_ports[] = {
    21, 22, 23, 25, 53, 80, 81, 88, 110, 111, 123, 135, 139, 143, 161, 179,
    389, 443, 445, 465, 500, 515, 543, 548, 554, 587, 631, 636, 873, 902,
    993, 995, 1024, 1080, 1194, 1400, 1433, 1521, 1723, 1883, 1900, 2049,
    2082, 2083, 2086, 2087, 2181, 2375, 2376, 3000, 3001, 3128, 3260, 3306,
    3389, 3478, 3689, 4000, 4444, 4567, 5000, 5001, 5060, 5061, 5222, 5353,
    5432, 5555, 5601, 5672, 5683, 5800, 5900, 5901, 6000, 6379, 6667, 7000,
    7001, 7070, 7777, 8000, 8006, 8008, 8009, 8010, 8060, 8080, 8081, 8083,
    8086, 8088, 8089, 8096, 8112, 8123, 8181, 8200, 8291, 8443, 8444, 8500,
    8686, 8765, 8800, 8880, 8883, 8888, 9000, 9001, 9090, 9091, 9100, 9200,
    9443, 9999, 10000, 11211, 27017, 32400, 32469, 49152, 51413, 62078,
};

#define PS_COMMON_COUNT (sizeof(s_common_ports) / sizeof(s_common_ports[0]))

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_state_lock;

static struct {
    bool     enabled;
    uint16_t rate;
    uint8_t  max_tier;

    uint8_t  tier;          /* ceiling: the highest tier this pass will reach */
    uint8_t  active_tier;   /* tier actually being probed right now           */
    uint16_t device_index;
    uint16_t device_count;
    uint32_t cursor;
    uint32_t tier_total;
    bool     running;
    uint32_t probes;
    uint32_t found;
    int64_t  cycle_started;

    uint16_t rescan_days;   /* re-scan this often, 0 = never                  */
    uint8_t  rescan_tier;   /* how deep a re-scan goes, capped at max_tier    */
    bool     rescanning;    /* the current device is a re-scan, not a first   */

    /* Set by portscan_rescan_device(); consumed by the task. */
    bool     jump_valid;
    uint8_t  jump_mac[6];
} s_ps;

/*
 * Read by the probe helpers to decide whether an open port is news. Only the
 * scan task writes it, and only between devices, so it needs no lock.
 */
static bool s_report_new_ports;

/*
 * The re-scan of one device, which works up through the tiers to the rescan
 * depth before the scanner moves on. It notes every port it finds open, so
 * that at the end any port on the old list that it re-checked and did not
 * find can be dropped. Scan task only.
 */
#define RS_MAX NETDASH_MAX_OPEN_PORTS
static struct {
    bool     active;
    uint8_t  goal;              /* last tier this re-scan covers           */
    uint8_t  mac[6];
    uint16_t before[RS_MAX];    /* the list as it stood when it started    */
    uint8_t  before_n;
    uint16_t found[RS_MAX];     /* open during this re-scan                */
    uint8_t  found_n;
    bool     found_overflow;    /* more open than we can track: keep all   */
} s_rs;

static void rs_note_open(const uint8_t mac[6], uint16_t port)
{
    if (!s_rs.active || memcmp(mac, s_rs.mac, 6) != 0) {
        return;
    }
    for (uint8_t i = 0; i < s_rs.found_n; i++) {
        if (s_rs.found[i] == port) {
            return;
        }
    }
    if (s_rs.found_n < RS_MAX) {
        s_rs.found[s_rs.found_n++] = port;
    } else {
        s_rs.found_overflow = true;
    }
}

static void state_lock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
}

static void reload_settings(void)
{
    netdash_settings_t cfg;
    settings_get(&cfg);

    state_lock();
    s_ps.enabled     = cfg.portscan_enabled;
    s_ps.rate        = cfg.portscan_rate > 0 ? cfg.portscan_rate : 1;
    s_ps.max_tier    = cfg.portscan_max_tier;
    s_ps.rescan_days = cfg.portscan_rescan_days;
    s_ps.rescan_tier = cfg.portscan_rescan_tier;
    if (s_ps.tier == 0) {
        s_ps.tier = PS_TIER_COMMON;
    }
    if (s_ps.tier > s_ps.max_tier) {
        s_ps.tier = PS_TIER_COMMON;   /* the user lowered the ceiling */
    }
    state_unlock();
}

/* ------------------------------------------------------------------------- */
/* Tier arithmetic                                                           */
/* ------------------------------------------------------------------------- */

static void record_open_port(const uint8_t mac[6], uint32_t ip, uint16_t port)
{
    rs_note_open(mac, port);   /* before the early return: known ports count too */
    if (!device_db_add_open_port(mac, port)) {
        return;   /* already knew about this one */
    }

    ESP_LOGI(TAG, "open %u.%u.%u.%u:%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff), (unsigned)port);

    state_lock();
    s_ps.found++;
    state_unlock();

    /*
     * Only on a re-probe. During the first pass over a device every port is
     * new by definition, and announcing all of them would bury the one case
     * this is for: something started listening that was not listening before.
     */
    if (s_report_new_ports) {
        const char *svc = netdash_port_service(port);
        char        text[NETDASH_NOTIF_TEXT];

        if (svc != NULL) {
            char label[32];
            netdash_service_label(svc, label, sizeof(label));
            snprintf(text, sizeof(text), "Port %u open (%s)", (unsigned)port, label);
        } else {
            snprintf(text, sizeof(text), "Port %u open", (unsigned)port);
        }
        notify_push(NETDASH_NOTIF_NEW_PORT, mac, ip, text);
    }
}

static uint32_t tier_total(uint8_t tier)
{
    switch (tier) {
    case PS_TIER_COMMON:    return (uint32_t)PS_COMMON_COUNT;
    case PS_TIER_WELLKNOWN: return 1024;              /* 1..1024      */
    case PS_TIER_FULL:      return 65535 - 1024;      /* 1025..65535  */
    default:                return 0;
    }
}

/* True when a re-scan reaching goal probed port. Tiers are cumulative. */
static bool tier_covers(uint8_t goal, uint16_t port)
{
    if (goal >= PS_TIER_FULL) {
        return true;
    }
    if (goal >= PS_TIER_WELLKNOWN && port <= 1024) {
        return true;
    }
    for (size_t i = 0; i < PS_COMMON_COUNT; i++) {
        if (s_common_ports[i] == port) {
            return true;
        }
    }
    return false;
}

/*
 * The end of a device's re-scan: drop the ports it had that this pass checked
 * and did not find open. Skipped when the device is not online now - a device
 * that left part-way would look as if every port had closed - or when it had
 * more open than could be tracked.
 */
static void rs_finish(uint32_t ip)
{
    netdash_device_t dev;
    if (!s_rs.active) {
        return;
    }
    s_rs.active = false;
    if (s_rs.found_overflow || !device_db_get_by_mac(s_rs.mac, &dev) ||
        !netdash_device_online(&dev)) {
        return;
    }
    for (uint8_t i = 0; i < s_rs.before_n; i++) {
        const uint16_t port = s_rs.before[i];
        bool           still = false;
        for (uint8_t k = 0; k < s_rs.found_n && !still; k++) {
            still = s_rs.found[k] == port;
        }
        if (still || !tier_covers(s_rs.goal, port) ||
            !device_db_remove_open_port(s_rs.mac, port)) {
            continue;
        }
        ESP_LOGI(TAG, "closed %u.%u.%u.%u:%u", (unsigned)(ip >> 24),
                 (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 8) & 0xff),
                 (unsigned)(ip & 0xff), (unsigned)port);
        const char *svc = netdash_port_service(port);
        char        text[NETDASH_NOTIF_TEXT];
        if (svc != NULL) {
            char label[32];
            netdash_service_label(svc, label, sizeof(label));
            snprintf(text, sizeof(text), "Port %u closed (%s)", (unsigned)port, label);
        } else {
            snprintf(text, sizeof(text), "Port %u closed", (unsigned)port);
        }
        notify_push(NETDASH_NOTIF_PORT_CLOSED, s_rs.mac, ip, text);
    }
}

static uint16_t tier_port_at(uint8_t tier, uint32_t index)
{
    switch (tier) {
    case PS_TIER_COMMON:    return s_common_ports[index];
    case PS_TIER_WELLKNOWN: return (uint16_t)(index + 1);
    case PS_TIER_FULL:      return (uint16_t)(index + 1025);
    default:                return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Probing                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    int      fd;
    uint16_t port;
} probe_t;

/*
 * Probes up to count ports on ip. Returns the number actually probed, and adds
 * every open port it finds to the database.
 */
static uint32_t probe_batch(const uint8_t mac[6], uint32_t ip,
                            uint8_t tier, uint32_t first_index, uint32_t count)
{
    probe_t probes[PS_BATCH];
    uint32_t n = 0;

    if (count > PS_BATCH) {
        count = PS_BATCH;
    }

    /*
     * Every slot starts closed. A probe that resolves synchronously still
     * advances n without filling its slot, and the cleanup loop below reads
     * every slot up to n - leaving them uninitialised would make it act on a
     * stack-garbage descriptor and it could close an unrelated socket.
     */
    for (uint32_t i = 0; i < PS_BATCH; i++) {
        probes[i].fd   = -1;
        probes[i].port = 0;
    }

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
    };
    dst.sin_addr.s_addr = htonl(ip);

    for (uint32_t i = 0; i < count; i++) {
        const uint16_t port = tier_port_at(tier, first_index + i);
        if (port == 0) {
            continue;
        }

        const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            /* Out of sockets: work with the batch we have. */
            break;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            break;
        }

        dst.sin_port = htons(port);
        const int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
        if (rc == 0) {
            /* Connected immediately, which on a LAN is entirely possible. */
            record_open_port(mac, ip, port);
            close(fd);
            n++;
            continue;
        }
        if (errno != EINPROGRESS) {
            close(fd);      /* refused or unreachable: port is not open */
            n++;
            continue;
        }

        probes[n].fd   = fd;
        probes[n].port = port;
        n++;
    }

    /* Nothing left pending: every probe resolved synchronously. */
    uint32_t pending = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (probes[i].fd >= 0) {
            pending++;
        }
    }
    if (pending == 0) {
        return n;
    }

    fd_set wset;
    FD_ZERO(&wset);
    int maxfd = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (probes[i].fd >= 0) {
            FD_SET(probes[i].fd, &wset);
            if (probes[i].fd > maxfd) {
                maxfd = probes[i].fd;
            }
        }
    }

    struct timeval tv = {
        .tv_sec  = PS_TIMEOUT_MS / 1000,
        .tv_usec = (PS_TIMEOUT_MS % 1000) * 1000,
    };
    const int ready = select(maxfd + 1, NULL, &wset, NULL, &tv);

    if (ready > 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (probes[i].fd < 0 || !FD_ISSET(probes[i].fd, &wset)) {
                continue;
            }
            /*
             * Writable only means the connect finished; SO_ERROR says whether
             * it succeeded. Without this check a refused port looks open.
             */
            int       err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(probes[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                record_open_port(mac, ip, probes[i].port);
            }
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        if (probes[i].fd >= 0) {
            close(probes[i].fd);
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Device selection                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Picks the next device to work on, and says which tier to scan it at.
 *
 * A device always advances through its own tiers in order, with the global
 * tier acting only as a ceiling, so `*use_tier` is the device's next unfinished
 * tier rather than the global one. That matters for a device that joins late:
 * without it, a device discovered while the network is working through tier 3
 * would be probed for ports 1025-65535 and marked finished, having never had
 * its common ports looked at.
 *
 * A device that has never been scanned also jumps the queue, so a device that
 * appears on the network gets its common ports within a couple of minutes
 * instead of waiting out a tier that can take days.
 *
 * Fills mac, ip and use_tier, and sets *index to that device's position.
 * False when every online device has reached the ceiling.
 */
static bool next_device_for_tier(uint8_t ceiling, uint16_t *index,
                                 uint8_t mac[6], uint32_t *ip, uint16_t *total,
                                 uint8_t *use_tier, uint16_t rescan_days, bool *is_rescan)
{
    bool found  = false;
    *is_rescan  = false;

    device_db_lock();
    const size_t count = device_db_count();
    if (total != NULL) {
        *total = (uint16_t)count;
    }

    /* Pass one: any device that has never been scanned at all. */
    for (size_t i = 0; i < count; i++) {
        netdash_device_t dev;
        if (!device_db_get_at(i, &dev)) {
            break;
        }
        if (!netdash_device_online(&dev) || dev.ip == 0) {
            continue;
        }

        netdash_ports_t ports;
        if (device_db_get_port_summary(dev.mac, &ports) && ports.tier == 0) {
            memcpy(mac, dev.mac, 6);
            *ip       = dev.ip;
            *index    = (uint16_t)i;
            *use_tier = PS_TIER_COMMON;
            found     = true;
            break;
        }
    }

    /* Pass two: resume the sweep through the list at the current ceiling. */
    if (!found) {
        for (size_t i = *index; i < count; i++) {
            netdash_device_t dev;
            if (!device_db_get_at(i, &dev)) {
                break;
            }
            if (!netdash_device_online(&dev) || dev.ip == 0) {
                continue;
            }

            netdash_ports_t ports;
            if (!device_db_get_port_summary(dev.mac, &ports) || ports.tier >= ceiling) {
                continue;   /* already at the ceiling */
            }

            memcpy(mac, dev.mac, 6);
            *ip       = dev.ip;
            *index    = (uint16_t)i;
            *use_tier = (uint8_t)(ports.tier + 1);
            found     = true;
            break;
        }
    }

    /*
     * Pass three: a device that finished long enough ago to be scanned again,
     * from the common ports up to the rescan depth. The stored list is not
     * cleared first: comparing against it is how a newly opened port is
     * noticed, and how one that has closed is dropped (see rs_finish()).
     */
    if (!found && rescan_days > 0) {
        const int64_t now = (int64_t)time(NULL);
        const int64_t age = (int64_t)rescan_days * 86400;

        for (size_t i = 0; i < count && now > 0; i++) {
            netdash_device_t dev;
            if (!device_db_get_at(i, &dev)) {
                break;
            }
            if (!netdash_device_online(&dev) || dev.ip == 0) {
                continue;
            }

            netdash_ports_t ports;
            if (!device_db_get_port_summary(dev.mac, &ports) || ports.tier < ceiling ||
                ports.last_scan == 0 || now - ports.last_scan < age) {
                continue;
            }

            memcpy(mac, dev.mac, 6);
            *ip        = dev.ip;
            *index     = (uint16_t)i;
            *use_tier  = PS_TIER_COMMON;
            *is_rescan = true;
            found      = true;
            break;
        }
    }
    device_db_unlock();
    return found;
}

/* ------------------------------------------------------------------------- */
/* Task                                                                      */
/* ------------------------------------------------------------------------- */

static void portscan_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(PS_START_DELAY_MS));

    uint8_t  mac[6]   = {0};
    uint32_t ip       = 0;
    bool     have_dev = false;
    uint8_t  dev_tier = PS_TIER_COMMON;   /* tier being scanned for this device */
    /*
     * Once every online device is done, the loop below still steps through
     * the tiers every PS_IDLE_MS looking for work. Announcing each of those
     * as "complete" filled the console with three lines every five seconds,
     * so a tier is only announced when it scanned something, and the idle
     * state is announced once.
     */
    bool     tier_worked  = false;
    bool     cycle_worked = false;
    bool     idle_said    = false;

    for (;;) {
        reload_settings();

        state_lock();
        const bool     enabled  = s_ps.enabled;
        const uint16_t rate     = s_ps.rate;
        const uint8_t  ceiling  = s_ps.tier;
        const uint8_t  max_tier = s_ps.max_tier;
        state_unlock();

        const wifi_mgr_mode_t mode = wifi_mgr_get_mode();
        if (!enabled || (mode != WIFI_MGR_MODE_STA && mode != WIFI_MGR_MODE_APSTA)) {
            have_dev = false;
            state_lock();
            s_ps.running = false;
            state_unlock();
            vTaskDelay(pdMS_TO_TICKS(PS_IDLE_MS));
            continue;
        }

        /* A rescan request jumps the queue. */
        state_lock();
        const bool jump = s_ps.jump_valid;
        if (jump) {
            memcpy(mac, s_ps.jump_mac, 6);
            s_ps.jump_valid = false;
            s_ps.cursor     = 0;
            have_dev        = false;
        }
        state_unlock();

        if (!have_dev) {
            state_lock();
            uint16_t index = s_ps.device_index;
            state_unlock();

            state_lock();
            const uint16_t rescan_days = s_ps.rescan_days;
            state_unlock();

            uint16_t count     = 0;
            bool     is_rescan = false;
            if (!next_device_for_tier(ceiling, &index, mac, &ip, &count, &dev_tier, rescan_days,
                                      &is_rescan)) {
                /* Tier finished for every device: advance, or start over. */
                state_lock();
                const uint8_t next = (s_ps.tier >= max_tier) ? PS_TIER_COMMON
                                                             : (uint8_t)(s_ps.tier + 1);
                if (next == PS_TIER_COMMON) {
                    s_ps.cycle_started = 0;   /* a fresh cycle begins */
                }
                if (tier_worked) {
                    ESP_LOGI(TAG, "tier %u complete across %u device(s), moving to tier %u",
                             (unsigned)s_ps.tier, (unsigned)count, (unsigned)next);
                }
                if (next == PS_TIER_COMMON && !cycle_worked && !idle_said) {
                    ESP_LOGI(TAG, "nothing to scan: all %u online device(s) done to tier %u; "
                                  "waiting for a new device or a re-scan",
                             (unsigned)count, (unsigned)max_tier);
                    idle_said = true;
                }
                tier_worked = false;
                if (next == PS_TIER_COMMON) {
                    cycle_worked = false;
                }
                s_ps.tier         = next;
                s_ps.device_index = 0;
                s_ps.cursor       = 0;
                s_ps.running      = false;
                state_unlock();

                if (next == PS_TIER_COMMON) {
                    /*
                     * A full pass just wrapped. Clearing the recorded tier
                     * would mean rescanning immediately, so instead wait out
                     * one idle period and let the freshness of the results
                     * come from the rescan endpoint or a device reappearing.
                     */
                    vTaskDelay(pdMS_TO_TICKS(PS_IDLE_MS));
                }
                continue;
            }

            state_lock();
            s_ps.device_index = index;
            s_ps.device_count = count;
            s_ps.active_tier  = dev_tier;
            s_ps.rescanning   = is_rescan;
            s_ps.tier_total   = tier_total(dev_tier);
            if (!jump) {
                s_ps.cursor = 0;
            }
            if (s_ps.cycle_started == 0) {
                s_ps.cycle_started = (int64_t)time(NULL);
            }
            state_unlock();
            s_report_new_ports = is_rescan;
            have_dev           = true;
            tier_worked        = true;
            cycle_worked       = true;
            idle_said          = false;

            if (is_rescan) {
                netdash_ports_t old;
                state_lock();
                const uint8_t depth = s_ps.rescan_tier < max_tier ? s_ps.rescan_tier : max_tier;
                state_unlock();
                memset(&s_rs, 0, sizeof(s_rs));
                s_rs.active = true;
                s_rs.goal   = depth < PS_TIER_COMMON ? PS_TIER_COMMON : depth;
                memcpy(s_rs.mac, mac, 6);
                if (device_db_get_ports(mac, &old)) {
                    s_rs.before_n = old.count > RS_MAX ? RS_MAX : old.count;
                    memcpy(s_rs.before, old.ports, s_rs.before_n * sizeof(s_rs.before[0]));
                }
            } else {
                s_rs.active = false;
            }
        }

        state_lock();
        const uint32_t cursor = s_ps.cursor;
        const uint32_t total  = s_ps.tier_total;
        s_ps.running          = true;
        state_unlock();

        if (cursor >= total && s_rs.active && memcmp(mac, s_rs.mac, 6) == 0 &&
            dev_tier < s_rs.goal) {
            dev_tier++;
            state_lock();
            s_ps.cursor      = 0;
            s_ps.active_tier = dev_tier;
            s_ps.tier_total  = tier_total(dev_tier);
            state_unlock();
            device_db_set_scan_progress(mac, dev_tier, 0, tier_total(dev_tier));
            continue;
        }
        if (cursor >= total) {
            if (s_rs.active && memcmp(mac, s_rs.mac, 6) == 0) {
                rs_finish(ip);
            }
            device_db_finish_tier(mac, dev_tier, (int64_t)time(NULL));
            state_lock();
            s_ps.device_index++;   /* move past the device just finished */
            s_ps.cursor = 0;
            state_unlock();
            have_dev = false;
            continue;
        }

        const int64_t  t0     = esp_timer_get_time();
        const uint32_t probed = probe_batch(mac, ip, dev_tier, cursor, total - cursor);

        if (probed == 0) {
            /* Could not get a socket; back off briefly and retry. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        state_lock();
        s_ps.cursor += probed;
        s_ps.probes += probed;
        const uint32_t new_cursor = s_ps.cursor;
        state_unlock();

        device_db_set_scan_progress(mac, dev_tier, new_cursor, total);

        /*
         * Pace to the configured probes per second. The batch already took
         * some time, usually the select() timeout, so only sleep the balance.
         */
        const int64_t want_us  = ((int64_t)probed * 1000000) / (rate > 0 ? rate : 1);
        const int64_t spent_us = esp_timer_get_time() - t0;
        if (want_us > spent_us) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((want_us - spent_us) / 1000)));
        } else {
            taskYIELD();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t portscan_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutex();
        if (s_state_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_ps.tier = PS_TIER_COMMON;
    reload_settings();

    if (xTaskCreate(portscan_task, "netdash_ports", 4096, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "port scanner started (%u common ports, tiers up to %u)",
             (unsigned)PS_COMMON_COUNT, (unsigned)s_ps.max_tier);
    return ESP_OK;
}

void portscan_get_status(portscan_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    state_lock();
    out->enabled       = s_ps.enabled;
    out->running       = s_ps.running;
    out->tier          = s_ps.active_tier != 0 ? s_ps.active_tier : s_ps.tier;
    out->ceiling       = s_ps.tier;
    out->max_tier      = s_ps.max_tier;
    out->rate          = s_ps.rate;
    out->device_index  = s_ps.device_index;
    out->device_count  = s_ps.device_count;
    out->cursor        = s_ps.cursor;
    out->tier_total    = s_ps.tier_total;
    out->probes        = s_ps.probes;
    out->found         = s_ps.found;
    out->cycle_started = s_ps.cycle_started;
    state_unlock();
}

esp_err_t portscan_rescan_device(const uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_db_clear_ports(mac);

    state_lock();
    memcpy(s_ps.jump_mac, mac, 6);
    s_ps.jump_valid = true;
    s_ps.tier       = PS_TIER_COMMON;
    s_ps.cursor     = 0;
    state_unlock();

    ESP_LOGI(TAG, "rescan queued for %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

void portscan_settings_changed(void)
{
    reload_settings();
}
