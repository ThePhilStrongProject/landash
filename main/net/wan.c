/*
 * WAN health monitor. See wan.h for what the two probes are for.
 *
 * The ICMP probe goes through esp_ping rather than a second raw socket of our
 * own: the scanner already owns one, and two raw ICMP PCBs both receive every
 * echo reply on the interface. esp_ping matches replies by identifier and
 * sequence, so the two cannot be confused for one another.
 */
#include "wan.h"

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#include "app_events.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "notify.h"
#include "ping/ping_sock.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "wan";

#define WAN_TASK_STACK  4096
#define WAN_TASK_PRIO   3
#define WAN_PING_TMO_MS 2000

/*
 * One failed check is a dropped packet; three in a row is an outage. Sitting
 * on it this long keeps a single lost echo off the LCD and out of the feed.
 */
#define WAN_FAIL_STREAK 3

static netdash_wan_t     s_state;
static SemaphoreHandle_t s_lock;
static volatile bool     s_check_now;
static uint8_t           s_fail_streak;
static uint8_t           s_ok_streak;

/*
 * Result of the check in flight, shared with the ping callbacks.
 *
 * Both this and the session below outlive any single check on purpose. The
 * obvious shape - create a session, wait, delete it, with the context on the
 * caller's stack - races: the ping task can still be inside recvfrom() when
 * the session is deleted, and its end callback then gives a semaphore that has
 * already been deleted, through a stack frame that no longer exists. That
 * asserts inside the queue code and reboots the dongle.
 *
 * Keeping both alive for the lifetime of the task removes the race entirely. A
 * callback that arrives late writes to memory that is still valid and gives a
 * semaphore that the next probe drains before it starts.
 */
typedef struct {
    SemaphoreHandle_t done;
    bool              ok;
    uint32_t          rtt_ms;
} ping_ctx_t;

static ping_ctx_t        s_ping;
static esp_ping_handle_t s_ping_hdl;
static char              s_ping_target[40];   /* what s_ping_hdl was built for */

/* ------------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    uint32_t    elapsed = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ctx->ok     = true;
    ctx->rtt_ms = elapsed;
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    xSemaphoreGive(ctx->done);
}

/* Resolves host to an lwIP address. Accepts a dotted quad without a lookup. */
static bool resolve_host(const char *host, ip_addr_t *out)
{
    ip4_addr_t v4;
    if (ip4addr_aton(host, &v4)) {
        ip_addr_set_ip4_u32(out, v4.addr);
        return true;
    }

    struct addrinfo  hints = {.ai_family = AF_INET, .ai_socktype = SOCK_RAW};
    struct addrinfo *res   = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
    ip_addr_set_ip4_u32(out, sa->sin_addr.s_addr);
    freeaddrinfo(res);
    return true;
}

/*
 * Makes sure s_ping_hdl is a session aimed at host. Only rebuilds it when the
 * configured target actually changes, which is a settings edit rather than
 * something that happens on every check. Caller is the wan task, which is the
 * only thing that ever starts or stops the session.
 */
static bool ensure_session(const char *host)
{
    if (s_ping_hdl != NULL && strcmp(s_ping_target, host) == 0) {
        return true;
    }

    ip_addr_t target;
    if (!resolve_host(host, &target)) {
        return false;
    }

    if (s_ping_hdl != NULL) {
        esp_ping_stop(s_ping_hdl);
        esp_ping_delete_session(s_ping_hdl);
        s_ping_hdl = NULL;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr       = target;
    cfg.count             = 1;
    cfg.timeout_ms        = WAN_PING_TMO_MS;
    cfg.interval_ms       = WAN_PING_TMO_MS;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end     = on_ping_end,
        .cb_args         = &s_ping,
    };

    if (esp_ping_new_session(&cfg, &cbs, &s_ping_hdl) != ESP_OK) {
        s_ping_hdl = NULL;
        return false;
    }
    snprintf(s_ping_target, sizeof(s_ping_target), "%s", host);
    return true;
}

static bool probe_icmp(const char *host, uint32_t *out_rtt_ms)
{
    if (!ensure_session(host)) {
        return false;
    }

    /* Drop anything a previous check's callback left behind, so a late give
       cannot be mistaken for this check finishing instantly. */
    while (xSemaphoreTake(s_ping.done, 0) == pdTRUE) {
    }
    s_ping.ok     = false;
    s_ping.rtt_ms = 0;

    if (esp_ping_start(s_ping_hdl) != ESP_OK) {
        return false;
    }

    /* The session ends itself after one echo; the wait is a backstop. */
    bool ok = false;
    if (xSemaphoreTake(s_ping.done, pdMS_TO_TICKS(WAN_PING_TMO_MS + 1000)) == pdTRUE) {
        ok = s_ping.ok;
        if (ok && out_rtt_ms != NULL) {
            *out_rtt_ms = s_ping.rtt_ms;
        }
    } else {
        /* It did not finish in time. Stop it so the next check starts clean;
           the session itself stays alive. */
        esp_ping_stop(s_ping_hdl);
    }
    return ok;
}

static bool probe_dns(const char *name)
{
    struct addrinfo  hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res   = NULL;

    if (getaddrinfo(name, NULL, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    freeaddrinfo(res);
    return true;
}

static netdash_wan_state_t classify(bool icmp_ok, bool dns_ok)
{
    if (icmp_ok && dns_ok) {
        return NETDASH_WAN_UP;
    }
    if (!icmp_ok && !dns_ok) {
        return NETDASH_WAN_DOWN;
    }
    return NETDASH_WAN_DEGRADED;
}

/* Applies a finished check, and announces it if the state actually moved. */
static void commit(bool icmp_ok, bool dns_ok, int16_t rtt_ms)
{
    const netdash_wan_state_t observed = classify(icmp_ok, dns_ok);
    const int64_t             now      = (int64_t)time(NULL);

    /*
     * Hold the last state until a run of checks agrees. A single lost echo on
     * a busy Wi-Fi link is not an outage, and flapping the LCD between "up"
     * and "down" every minute would make the display worthless.
     */
    if (observed == NETDASH_WAN_UP) {
        s_ok_streak++;
        s_fail_streak = 0;
    } else {
        s_fail_streak++;
        s_ok_streak = 0;
    }

    lock();
    const netdash_wan_state_t previous = (netdash_wan_state_t)s_state.state;
    netdash_wan_state_t       next     = previous;

    if (observed == NETDASH_WAN_UP) {
        next = NETDASH_WAN_UP;
    } else if (s_fail_streak >= WAN_FAIL_STREAK || previous != NETDASH_WAN_UP) {
        next = observed;
    }

    s_state.icmp_ok    = icmp_ok;
    s_state.dns_ok     = dns_ok;
    s_state.last_check = now;
    s_state.checks++;
    if (observed != NETDASH_WAN_UP) {
        s_state.failures++;
    }
    if (rtt_ms >= 0) {
        s_state.rtt_ms = rtt_ms;
    }
    const bool changed = (next != previous);
    if (changed) {
        s_state.state      = (uint8_t)next;
        s_state.changed_at = now;
    }
    unlock();

    if (!changed) {
        return;
    }

    ESP_LOGI(TAG, "%s -> %s (icmp=%d dns=%d)", netdash_wan_state_name(previous),
             netdash_wan_state_name(next), (int)icmp_ok, (int)dns_ok);

    esp_event_post(NETDASH_EVENT, NETDASH_EVENT_WAN_CHANGED, NULL, 0, 0);

    if (next == NETDASH_WAN_UP) {
        if (previous != NETDASH_WAN_UNKNOWN) {
            notify_push(NETDASH_NOTIF_WAN_UP, NULL, 0, "Internet is back");
        }
    } else if (next == NETDASH_WAN_DOWN) {
        notify_push(NETDASH_NOTIF_WAN_DOWN, NULL, 0, "No route to the internet");
    } else {
        notify_push(NETDASH_NOTIF_WAN_DOWN, NULL, 0,
                    icmp_ok ? "DNS is not resolving" : "Internet unreachable, DNS still answers");
    }
}

static void wan_task(void *arg)
{
    (void)arg;

    /* Let the station associate and SNTP settle before the first verdict. */
    vTaskDelay(pdMS_TO_TICKS(10000));

    int64_t next_due_us = 0;

    for (;;) {
        netdash_settings_t cfg;
        settings_get(&cfg);

        const wifi_mgr_mode_t mode = wifi_mgr_get_mode();
        const bool            have_lan =
            (mode == WIFI_MGR_MODE_STA || mode == WIFI_MGR_MODE_APSTA) &&
            wifi_mgr_get_gateway() != 0;

        if (!cfg.wan_enabled || !have_lan) {
            /* Report "unknown" rather than "down" when we are in no position
               to know - an AP-mode dongle has no WAN to speak of. */
            lock();
            if (s_state.state != NETDASH_WAN_UNKNOWN) {
                s_state.state      = NETDASH_WAN_UNKNOWN;
                s_state.changed_at = (int64_t)time(NULL);
            }
            unlock();
            s_fail_streak = 0;
            s_ok_streak   = 0;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        if (s_check_now || now_us >= next_due_us) {
            s_check_now = false;

            uint32_t rtt   = 0;
            const bool ping_ok = probe_icmp(cfg.wan_ping_host, &rtt);
            const bool dns_ok  = probe_dns(cfg.wan_dns_probe);

            commit(ping_ok, dns_ok, ping_ok ? (int16_t)(rtt > 32767 ? 32767 : rtt) : -1);

            next_due_us = esp_timer_get_time() + (int64_t)cfg.wan_interval_s * 1000000;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------------------- */

esp_err_t wan_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ping.done == NULL) {
        s_ping.done = xSemaphoreCreateBinary();
        if (s_ping.done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.state  = NETDASH_WAN_UNKNOWN;
    s_state.rtt_ms = -1;

    if (xTaskCreate(wan_task, "wan", WAN_TASK_STACK, NULL, WAN_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the monitor task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wan_get(netdash_wan_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    *out = s_state;
    unlock();
}

void wan_check_now(void)
{
    s_check_now = true;
}

const char *netdash_wan_state_name(netdash_wan_state_t state)
{
    switch (state) {
    case NETDASH_WAN_UP:       return "up";
    case NETDASH_WAN_DEGRADED: return "degraded";
    case NETDASH_WAN_DOWN:     return "down";
    default:                   return "unknown";
    }
}
