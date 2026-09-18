/*
 * mDNS discovery, plus the plumbing shared by all four discovery sources.
 *
 * Design notes
 * ------------
 * - One task drives every source. It wakes on NETDASH_EVENT SCAN_DONE (and 20 s
 *   after the STA gets an IP), then runs mDNS, SSDP, rDNS and NBNS in that
 *   order. A whole pass starts at most once every DISC_MIN_PERIOD_US and is
 *   skipped unless the Wi-Fi manager is associated as a station.
 * - Every source starts from a snapshot of the online devices taken under
 *   device_db_lock() and released before any socket work, so the database lock
 *   is never held across network I/O. The snapshot's mutex doubles as the
 *   mutual exclusion between sources, which is what makes their per-source
 *   static packet buffers safe.
 * - The mDNS responder itself is owned by wifi_mgr (it calls mdns_init()), so
 *   this module only ever issues queries.
 */
#include "disc_mdns.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"

#include "lwip/inet.h"

#include "mdns.h"

#include "app_events.h"
#include "device_db.h"
#include "disc_nbns.h"
#include "disc_rdns.h"
#include "disc_ssdp.h"
#include "wifi_mgr.h"

static const char *TAG = "disc_mdns";

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

#define DISC_TASK_STACK        6144
#define DISC_TASK_PRIO         3

/* A full pass (all four sources) starts at most this often. */
#define DISC_MIN_PERIOD_US     (120LL * 1000 * 1000)

/* First pass runs this long after the station gets an IP. */
#define DISC_FIRST_DELAY_MS    20000

/* How often the task re-evaluates a deferred request. */
#define DISC_TICK_MS           2000

/* Per-service query budget: 13 services x 1.5 s keeps a pass under ~20 s. */
#define MDNS_QUERY_TIMEOUT_MS  1500
#define MDNS_MAX_RESULTS       20

/* ------------------------------------------------------------------------- */
/* Shared snapshot                                                           */
/* ------------------------------------------------------------------------- */

static disc_snap_t       s_snap[NETDASH_MAX_DEVICES];
static SemaphoreHandle_t s_snap_mux;

size_t disc_snapshot_acquire(const disc_snap_t **out)
{
    if (out == NULL) {
        return 0;
    }
    *out = s_snap;

    if (s_snap_mux == NULL) {
        ESP_LOGW(TAG, "snapshot requested before disc_*_init()");
        return 0;
    }
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);

    size_t n = 0;
    device_db_lock();
    const size_t total = device_db_count();
    for (size_t i = 0; i < total && n < NETDASH_MAX_DEVICES; i++) {
        netdash_device_t dev;
        if (!device_db_get_at(i, &dev)) {
            break;
        }
        if (!netdash_device_online(&dev) || dev.ip == 0) {
            continue;
        }
        memcpy(s_snap[n].mac, dev.mac, sizeof(s_snap[n].mac));
        s_snap[n].ip       = dev.ip;
        s_snap[n].name_src = dev.name_src;
        n++;
    }
    device_db_unlock();

    ESP_LOGD(TAG, "snapshot: %u online devices", (unsigned)n);
    return n;
}

void disc_snapshot_release(void)
{
    if (s_snap_mux != NULL) {
        xSemaphoreGive(s_snap_mux);
    }
}

const disc_snap_t *disc_snapshot_find_ip(const disc_snap_t *snap, size_t count, uint32_t ip)
{
    if (snap == NULL || ip == 0) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (snap[i].ip == ip) {
            return &snap[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Shared task                                                               */
/* ------------------------------------------------------------------------- */

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_wake;
static volatile bool     s_pending;
static volatile int64_t  s_not_before_us;
static int64_t           s_last_pass_us = -DISC_MIN_PERIOD_US;

static void disc_run_pass(void)
{
    const int64_t t0 = esp_timer_get_time();

    (void)disc_mdns_run_once();
    (void)disc_ssdp_run_once();
    (void)disc_rdns_run_once();
    (void)disc_nbns_run_once();

    ESP_LOGI(TAG, "discovery pass done in %d ms",
             (int)((esp_timer_get_time() - t0) / 1000));
}

static void disc_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)xSemaphoreTake(s_wake, pdMS_TO_TICKS(DISC_TICK_MS));
        if (!s_pending) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (now < s_not_before_us) {
            continue;                       /* still inside the post-IP delay  */
        }
        if (now - s_last_pass_us < DISC_MIN_PERIOD_US) {
            continue;                       /* rate limited, stays pending     */
        }

        const wifi_mgr_mode_t mode = wifi_mgr_get_mode();
        if (mode != WIFI_MGR_MODE_STA && mode != WIFI_MGR_MODE_APSTA) {
            s_pending = false;              /* no LAN to discover on           */
            ESP_LOGD(TAG, "pass skipped: not a station");
            continue;
        }

        s_pending      = false;
        s_last_pass_us = now;
        disc_run_pass();
    }
}

static void disc_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == NETDASH_EVENT_WIFI_STA_GOT_IP) {
        s_not_before_us = esp_timer_get_time() + (int64_t)DISC_FIRST_DELAY_MS * 1000;
    }
    s_pending = true;
    if (s_wake != NULL) {
        xSemaphoreGive(s_wake);
    }
}

esp_err_t disc_task_ensure(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (s_snap_mux == NULL) {
        s_snap_mux = xSemaphoreCreateMutex();
    }
    if (s_wake == NULL) {
        s_wake = xSemaphoreCreateBinary();
    }
    if (s_snap_mux == NULL || s_wake == NULL) {
        ESP_LOGE(TAG, "semaphore allocation failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(
        NETDASH_EVENT, NETDASH_EVENT_SCAN_DONE, disc_event_handler, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            NETDASH_EVENT, NETDASH_EVENT_WIFI_STA_GOT_IP, disc_event_handler, NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event registration failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(disc_task, "netdash_disc", DISC_TASK_STACK, NULL,
                    DISC_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "discovery task started");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* mDNS source                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *service;
    const char *proto;
    uint16_t    bits;
} mdns_service_t;

/*
 * The browse list. _esphomelib devices always run a web server as well, and
 * _androidtvremote2 only tells us the box is an Android TV, which classify.c
 * derives from the hostname, so it contributes no service bit.
 */
static const mdns_service_t s_services[] = {
    { "_http",              "_tcp", NETDASH_SVC_HTTP },
    { "_https",             "_tcp", NETDASH_SVC_HTTPS },
    { "_smb",               "_tcp", NETDASH_SVC_SMB },
    { "_ssh",               "_tcp", NETDASH_SVC_SSH },
    { "_hap",               "_tcp", NETDASH_SVC_HAP },
    { "_home-assistant",    "_tcp", NETDASH_SVC_HA },
    { "_googlecast",        "_tcp", NETDASH_SVC_CAST },
    { "_airplay",           "_tcp", NETDASH_SVC_AIRPLAY },
    { "_workstation",       "_tcp", NETDASH_SVC_WORKSTATION },
    { "_ipp",               "_tcp", NETDASH_SVC_PRINTER },
    { "_printer",           "_tcp", NETDASH_SVC_PRINTER },
    { "_esphomelib",        "_tcp", NETDASH_SVC_HA | NETDASH_SVC_HTTP },
    { "_androidtvremote2",  "_tcp", 0 },
};

#define MDNS_SERVICE_COUNT (sizeof(s_services) / sizeof(s_services[0]))

/*
 * Merges one query result list. Returns the number of results that carried an
 * IPv4 address; *matched gets the number that also matched a known device.
 */
static unsigned mdns_merge_results(const mdns_result_t *results, uint16_t bits,
                                   const disc_snap_t *snap, size_t snap_count,
                                   unsigned *matched)
{
    unsigned answers = 0;

    for (const mdns_result_t *r = results; r != NULL; r = r->next) {
        bool has_v4 = false;

        for (const mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                continue;
            }
            const uint32_t ip = ntohl(a->addr.u_addr.ip4.addr);
            has_v4 = true;

            const disc_snap_t *dev = disc_snapshot_find_ip(snap, snap_count, ip);
            if (dev == NULL) {
                ESP_LOGD(TAG, "no device for " IPSTR, IP2STR(&a->addr.u_addr.ip4));
                continue;
            }

            if (r->hostname != NULL && r->hostname[0] != '\0') {
                (void)device_db_set_hostname(dev->mac, r->hostname, NETDASH_NAME_SRC_MDNS);
            }
            if (bits != 0) {
                (void)device_db_set_services(dev->mac, bits);
            }
            ESP_LOGD(TAG, "matched " IPSTR " -> %s", IP2STR(&a->addr.u_addr.ip4),
                     r->hostname != NULL ? r->hostname : "(no hostname)");
            (*matched)++;
        }

        if (has_v4) {
            answers++;
        }
    }

    return answers;
}

esp_err_t disc_mdns_run_once(void)
{
    const disc_snap_t *snap  = NULL;
    const size_t       count = disc_snapshot_acquire(&snap);
    if (count == 0) {
        disc_snapshot_release();
        ESP_LOGD(TAG, "nothing online, skipping");
        return ESP_OK;
    }

    unsigned  answers = 0;
    unsigned  matched = 0;
    esp_err_t result  = ESP_OK;

    for (size_t i = 0; i < MDNS_SERVICE_COUNT; i++) {
        mdns_result_t *results = NULL;
        esp_err_t      err     = mdns_query_ptr(s_services[i].service, s_services[i].proto,
                                                MDNS_QUERY_TIMEOUT_MS, MDNS_MAX_RESULTS, &results);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "%s%s query failed: %s", s_services[i].service,
                     s_services[i].proto, esp_err_to_name(err));
            if (err == ESP_ERR_INVALID_STATE) {
                result = err;       /* responder is not up, the rest will fail too */
                break;
            }
            continue;
        }

        answers += mdns_merge_results(results, s_services[i].bits, snap, count, &matched);
        mdns_query_results_free(results);
    }

    disc_snapshot_release();
    ESP_LOGI(TAG, "mdns: %u answers, %u matched", answers, matched);
    return result;
}

esp_err_t disc_mdns_init(void)
{
    return disc_task_ensure();
}
