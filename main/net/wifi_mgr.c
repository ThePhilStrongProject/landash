/*
 * NetDash Wi-Fi manager.
 *
 * Owns every esp_wifi call in the firmware. A single worker task drives a
 * small state machine; the esp_event handlers and the esp_timer callbacks only
 * push messages onto its queue, so nothing blocks the system event task and
 * public entry points (called from httpd, the button task, ...) never touch
 * the driver directly.
 *
 * Behaviour:
 *   creds stored  -> STA, connect, retry with 1/2/4/8/16 s backoff for 60 s.
 *                    Still down after that -> APSTA: the setup AP comes up and
 *                    STA keeps retrying every 120 s. Once STA gets an IP the AP
 *                    is torn down 60 s later.
 *   no creds      -> AP only, "NetDash-XXXX" / settings.ap_pass on 192.168.4.1.
 *
 * After the STA gets an IP, SNTP and the mDNS advertisement are started.
 */
#include "wifi_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mdns.h"

#include "app_events.h"
#include "settings.h"

static const char *TAG = "wifi_mgr";

/* ------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* ------------------------------------------------------------------------- */

#define AP_CHANNEL          1
#define AP_MAX_CLIENTS      4
#define AP_PASS_MIN_LEN     8

/* Fast-retry backoff while the STA-only window is open. */
static const uint32_t k_backoff_ms[] = {1000, 2000, 4000, 8000, 16000};

#define STA_WINDOW_MS       60000   /* STA-only before the AP fallback opens  */
#define STA_SLOW_RETRY_MS   120000  /* retry cadence once the AP is up        */
#define AP_LINGER_MS        60000   /* AP stays up this long after the STA IP */

#define TASK_STACK          4096
#define TASK_PRIO           5
#define QUEUE_LEN           12

#define SCAN_MAX_RECORDS    32

/* ------------------------------------------------------------------------- */
/* Worker messages                                                            */
/* ------------------------------------------------------------------------- */

typedef enum {
    MSG_START = 0,          /* first bring-up                                 */
    MSG_APPLY,              /* settings changed                               */
    MSG_STA_STARTED,
    MSG_STA_STOPPED,
    MSG_STA_CONNECTED,
    MSG_STA_DISCONNECTED,   /* arg = reason                                   */
    MSG_STA_GOT_IP,
    MSG_STA_LOST_IP,
    MSG_AP_STARTED,
    MSG_AP_STOPPED,
    MSG_RETRY_TIMER,
    MSG_FALLBACK_TIMER,
    MSG_AP_LINGER_TIMER,
} msg_id_t;

typedef struct {
    uint8_t  id;
    uint32_t arg;
} wifi_msg_t;

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

/* Owned by the worker task only. */
static esp_netif_t        *s_sta_netif;
static esp_netif_t        *s_ap_netif;
static netdash_settings_t  s_cfg;           /* last applied settings          */
static bool                s_first_apply = true;
static bool                s_sta_enabled;   /* we want to be associated       */
static bool                s_sta_iface_up;  /* WIFI_EVENT_STA_START seen      */
static bool                s_ap_fallback;   /* AP is up because STA failed    */
static bool                s_wifi_started;
static uint8_t             s_attempt;
static bool                s_sntp_started;
static bool                s_mdns_started;
static char                s_mdns_hostname[32];
static char                s_ntp_server[64];

static esp_timer_handle_t  s_retry_timer;
static esp_timer_handle_t  s_fallback_timer;
static esp_timer_handle_t  s_linger_timer;

static QueueHandle_t       s_queue;
static SemaphoreHandle_t   s_state_lock;    /* guards the snapshot below      */
static SemaphoreHandle_t   s_scan_lock;

/* Snapshot read by the getters from any task. */
static wifi_mgr_mode_t     s_mode = WIFI_MGR_MODE_OFF;
static bool                s_has_ip;
static esp_netif_ip_info_t s_sta_ip;
static esp_netif_ip_info_t s_ap_ip;
static bool                s_ap_ip_valid;
static char                s_ssid[33];
static char                s_ap_ssid[33];
static uint8_t             s_base_mac[6];

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

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

/*
 * Copies a C string into a fixed driver field that does not have to be
 * NUL-terminated when it is exactly full. strncpy() would trip
 * -Wstringop-truncation here because the settings buffers are one byte larger.
 */
static void copy_field(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap) {
        n = cap;
    }
    memset(dst, 0, cap);
    memcpy(dst, src, n);
}

static void post_msg(msg_id_t id, uint32_t arg)
{
    if (s_queue == NULL) {
        return;
    }
    const wifi_msg_t msg = {.id = (uint8_t)id, .arg = arg};
    /* Never block: the callers are the system event task and esp_timer. */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped message %d", (int)id);
    }
}

static void set_mode_snapshot(wifi_mgr_mode_t mode)
{
    state_lock();
    s_mode = mode;
    state_unlock();
}

static wifi_mgr_mode_t mode_from_driver(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    if (!s_wifi_started || esp_wifi_get_mode(&m) != ESP_OK) {
        return WIFI_MGR_MODE_OFF;
    }
    switch (m) {
    case WIFI_MODE_STA:   return WIFI_MGR_MODE_STA;
    case WIFI_MODE_AP:    return WIFI_MGR_MODE_AP;
    case WIFI_MODE_APSTA: return WIFI_MGR_MODE_APSTA;
    default:              return WIFI_MGR_MODE_OFF;
    }
}

static void refresh_mode_snapshot(void)
{
    set_mode_snapshot(mode_from_driver());
}

static void timer_stop(esp_timer_handle_t t)
{
    if (t != NULL) {
        esp_timer_stop(t);      /* ESP_ERR_INVALID_STATE when idle: ignore */
    }
}

static void timer_restart(esp_timer_handle_t t, uint32_t ms)
{
    if (t == NULL) {
        return;
    }
    esp_timer_stop(t);
    esp_err_t err = esp_timer_start_once(t, (uint64_t)ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer start failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* NetDash events                                                             */
/* ------------------------------------------------------------------------- */

static void post_netdash_wifi_event(netdash_event_id_t id, uint32_t ip_host_order)
{
    netdash_wifi_event_t ev = {0};
    ev.ip = ip_host_order;
    state_lock();
    memcpy(ev.ssid, s_ssid, sizeof(ev.ssid));
    state_unlock();
    ev.rssi = wifi_mgr_get_rssi();

    esp_err_t err = esp_event_post(NETDASH_EVENT, (int32_t)id, &ev, sizeof(ev), 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event post failed: %s", esp_err_to_name(err));
    }
}

static void post_netdash_ap_event(netdash_event_id_t id)
{
    esp_err_t err = esp_event_post(NETDASH_EVENT, (int32_t)id, NULL, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event post failed: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* SNTP                                                                       */
/* ------------------------------------------------------------------------- */

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "time synced");
    esp_event_post(NETDASH_EVENT, (int32_t)NETDASH_EVENT_TIME_SYNCED, NULL, 0, 0);
}

static void apply_timezone(void)
{
    const char *tz = (s_cfg.tz[0] != '\0') ? s_cfg.tz : "UTC0";
    setenv("TZ", tz, 1);
    tzset();
}

static void sntp_stop(void)
{
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }
}

static void sntp_start(void)
{
    if (s_sntp_started) {
        return;
    }
    if (s_cfg.ntp_server[0] == '\0') {
        ESP_LOGW(TAG, "no NTP server configured, clock stays unset");
        return;
    }

    /* The config keeps a pointer to the server string, so it must outlive it. */
    strncpy(s_ntp_server, s_cfg.ntp_server, sizeof(s_ntp_server) - 1);
    s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    cfg.wait_for_sync = false;
    cfg.start         = true;
    cfg.sync_cb       = sntp_sync_cb;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (%s, TZ=%s)", s_ntp_server, s_cfg.tz);
}

/* ------------------------------------------------------------------------- */
/* mDNS advertisement                                                         */
/* ------------------------------------------------------------------------- */

static void mdns_stop(void)
{
    if (s_mdns_started) {
        mdns_free();
        s_mdns_started = false;
        s_mdns_hostname[0] = '\0';
    }
}

static void mdns_start(void)
{
    if (s_mdns_started) {
        return;
    }
    const char *host = (s_cfg.hostname[0] != '\0') ? s_cfg.hostname : "netdash";

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_started = true;
    strncpy(s_mdns_hostname, host, sizeof(s_mdns_hostname) - 1);
    s_mdns_hostname[sizeof(s_mdns_hostname) - 1] = '\0';

    err = mdns_hostname_set(s_mdns_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
    }
    err = mdns_instance_name_set("NetDash");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
    }

    mdns_txt_item_t txt[] = {
        {"path", "/"},
    };
    err = mdns_service_add("NetDash", "_http", "_tcp", 80, txt, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "mDNS up: %s.local (_http._tcp:80)", s_mdns_hostname);
}

/* Restarts mDNS when the configured hostname no longer matches. */
static void mdns_refresh(void)
{
    if (!s_mdns_started) {
        return;
    }
    const char *host = (s_cfg.hostname[0] != '\0') ? s_cfg.hostname : "netdash";
    if (strcmp(host, s_mdns_hostname) == 0) {
        return;
    }
    ESP_LOGI(TAG, "hostname changed %s -> %s, restarting mDNS", s_mdns_hostname, host);
    mdns_stop();
    mdns_start();
}

/* ------------------------------------------------------------------------- */
/* Driver configuration                                                       */
/* ------------------------------------------------------------------------- */

static void apply_hostname(void)
{
    const char *host = (s_cfg.hostname[0] != '\0') ? s_cfg.hostname : "netdash";
    esp_err_t   err;

    if (s_sta_netif != NULL) {
        err = esp_netif_set_hostname(s_sta_netif, host);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA hostname failed: %s", esp_err_to_name(err));
        }
    }
    if (s_ap_netif != NULL) {
        err = esp_netif_set_hostname(s_ap_netif, host);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AP hostname failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t write_sta_config(void)
{
    wifi_config_t cfg = {0};

    copy_field(cfg.sta.ssid, sizeof(cfg.sta.ssid), s_cfg.wifi_ssid);
    copy_field(cfg.sta.password, sizeof(cfg.sta.password), s_cfg.wifi_pass);

    /* Accept whatever the AP offers; the stock threshold rejects open APs. */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;
    /* AiMesh networks repeat one SSID: pick the strongest BSSID, not the first. */
    cfg.sta.scan_method        = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t write_ap_config(void)
{
    wifi_config_t cfg = {0};

    copy_field(cfg.ap.ssid, sizeof(cfg.ap.ssid), s_ap_ssid);
    cfg.ap.ssid_len       = (uint8_t)strlen(s_ap_ssid);
    cfg.ap.channel        = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CLIENTS;
    cfg.ap.beacon_interval = 100;

    if (strlen(s_cfg.ap_pass) >= AP_PASS_MIN_LEN) {
        copy_field(cfg.ap.password, sizeof(cfg.ap.password), s_cfg.ap_pass);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ESP_LOGW(TAG, "AP password too short, starting an open AP");
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Moves the driver to mode and (re)writes the configs the new mode needs. */
static esp_err_t set_mode(wifi_mode_t mode)
{
    wifi_mode_t cur = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur) == ESP_OK && cur == mode) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(%d) failed: %s", (int)mode, esp_err_to_name(err));
        return err;
    }
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        write_ap_config();
    }
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        write_sta_config();
    }
    refresh_mode_snapshot();
    return ESP_OK;
}

static bool driver_has_ap(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    return (esp_wifi_get_mode(&m) == ESP_OK) && (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
}

/* Fires esp_wifi_connect() only when the STA interface is actually running. */
static void try_connect(void)
{
    if (!s_sta_enabled || !s_wifi_started || !s_sta_iface_up) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
    }
}

/* Restarts the fast-retry window: immediate attempt, AP fallback armed. */
static void begin_sta_window(void)
{
    s_attempt = 0;
    timer_stop(s_retry_timer);
    timer_stop(s_linger_timer);
    if (!s_ap_fallback) {
        timer_restart(s_fallback_timer, STA_WINDOW_MS);
    }
    try_connect();
}

static void schedule_retry(void)
{
    uint32_t delay_ms;
    if (s_ap_fallback) {
        delay_ms = STA_SLOW_RETRY_MS;
    } else {
        const size_t n = sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0]);
        delay_ms = k_backoff_ms[(s_attempt < n) ? s_attempt : (n - 1)];
        if (s_attempt < 0xFF) {
            s_attempt++;
        }
    }
    ESP_LOGI(TAG, "retrying STA in %u ms", (unsigned)delay_ms);
    timer_restart(s_retry_timer, delay_ms);
}

/* ------------------------------------------------------------------------- */
/* esp_event handlers (system event task): translate and forward only          */
/* ------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        post_msg(MSG_STA_STARTED, 0);
        break;
    case WIFI_EVENT_STA_STOP:
        post_msg(MSG_STA_STOPPED, 0);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        post_msg(MSG_STA_CONNECTED, 0);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        post_msg(MSG_STA_DISCONNECTED, (d != NULL) ? d->reason : 0);
        break;
    }
    case WIFI_EVENT_AP_START:
        post_msg(MSG_AP_STARTED, 0);
        break;
    case WIFI_EVENT_AP_STOP:
        post_msg(MSG_AP_STOPPED, 0);
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *d = (const wifi_event_ap_staconnected_t *)data;
        if (d != NULL) {
            ESP_LOGI(TAG, "AP client joined " MACSTR, MAC2STR(d->mac));
        }
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *d = (const wifi_event_ap_stadisconnected_t *)data;
        if (d != NULL) {
            ESP_LOGI(TAG, "AP client left " MACSTR, MAC2STR(d->mac));
        }
        break;
    }
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case IP_EVENT_STA_GOT_IP: {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        if (e != NULL) {
            state_lock();
            s_sta_ip = e->ip_info;
            s_has_ip = true;
            state_unlock();
        }
        post_msg(MSG_STA_GOT_IP, 0);
        break;
    }
    case IP_EVENT_STA_LOST_IP:
        state_lock();
        s_has_ip = false;
        memset(&s_sta_ip, 0, sizeof(s_sta_ip));
        state_unlock();
        post_msg(MSG_STA_LOST_IP, 0);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* esp_timer callbacks                                                        */
/* ------------------------------------------------------------------------- */

static void retry_timer_cb(void *arg)    { (void)arg; post_msg(MSG_RETRY_TIMER, 0); }
static void fallback_timer_cb(void *arg) { (void)arg; post_msg(MSG_FALLBACK_TIMER, 0); }
static void linger_timer_cb(void *arg)   { (void)arg; post_msg(MSG_AP_LINGER_TIMER, 0); }

/* ------------------------------------------------------------------------- */
/* State machine                                                              */
/* ------------------------------------------------------------------------- */

static void start_driver(wifi_mode_t mode)
{
    esp_err_t err;

    if (!s_wifi_started) {
        err = esp_wifi_set_mode(mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
            return;
        }
        if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
            write_ap_config();
        }
        if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
            write_sta_config();
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return;
        }
        s_wifi_started = true;
        /* Min-modem keeps the mDNS responder and the web UI snappy. */
        err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_ps failed: %s", esp_err_to_name(err));
        }
    } else {
        set_mode(mode);
    }
    refresh_mode_snapshot();
}

/* Brings the fallback / setup AP up without disturbing the STA. */
static void ensure_ap_up(void)
{
    if (driver_has_ap()) {
        return;
    }
    set_mode(s_sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP);
}

static void handle_apply(void)
{
    netdash_settings_t ns;
    settings_get(&ns);

    const bool first = s_first_apply;
    const bool host_changed =
        first || strcmp(ns.hostname, s_cfg.hostname) != 0;
    const bool creds_changed =
        first || strcmp(ns.wifi_ssid, s_cfg.wifi_ssid) != 0 ||
        strcmp(ns.wifi_pass, s_cfg.wifi_pass) != 0;
    const bool ap_pass_changed =
        !first && strcmp(ns.ap_pass, s_cfg.ap_pass) != 0;
    const bool ntp_changed =
        !first && (strcmp(ns.ntp_server, s_cfg.ntp_server) != 0 ||
                   strcmp(ns.tz, s_cfg.tz) != 0);

    s_cfg = ns;
    s_first_apply = false;

    const bool want_sta = (s_cfg.wifi_ssid[0] != '\0');

    if (host_changed) {
        apply_hostname();
        mdns_refresh();
    }
    if (ntp_changed) {
        apply_timezone();
        sntp_stop();
        state_lock();
        const bool up = s_has_ip;
        state_unlock();
        if (up) {
            sntp_start();
        }
    }

    if (first) {
        s_sta_enabled = want_sta;
        if (want_sta) {
            ESP_LOGI(TAG, "credentials found, joining \"%s\"", s_cfg.wifi_ssid);
            start_driver(WIFI_MODE_STA);
            /* The connect happens on WIFI_EVENT_STA_START. */
            s_attempt = 0;
            timer_restart(s_fallback_timer, STA_WINDOW_MS);
        } else {
            ESP_LOGI(TAG, "no credentials, setup AP \"%s\" on 192.168.4.1", s_ap_ssid);
            start_driver(WIFI_MODE_AP);
        }
        return;
    }

    if (!want_sta) {
        /* Credentials wiped: drop to AP only. */
        s_sta_enabled = false;
        timer_stop(s_retry_timer);
        timer_stop(s_fallback_timer);
        timer_stop(s_linger_timer);
        esp_wifi_disconnect();
        state_lock();
        s_ssid[0] = '\0';
        s_has_ip  = false;
        memset(&s_sta_ip, 0, sizeof(s_sta_ip));
        state_unlock();
        s_ap_fallback = false;
        start_driver(WIFI_MODE_AP);
        ESP_LOGI(TAG, "credentials cleared, setup AP \"%s\" is up", s_ap_ssid);
        return;
    }

    if (ap_pass_changed && driver_has_ap()) {
        write_ap_config();
    }

    if (!creds_changed) {
        return;
    }

    /*
     * New credentials. The AP (if any) is kept so the browser session that
     * submitted them survives; it is torn down 60 s after the STA gets an IP.
     */
    ESP_LOGI(TAG, "new credentials, reconnecting to \"%s\"", s_cfg.wifi_ssid);
    s_sta_enabled = true;
    state_lock();
    s_ssid[0] = '\0';
    s_has_ip  = false;
    memset(&s_sta_ip, 0, sizeof(s_sta_ip));
    state_unlock();

    if (driver_has_ap()) {
        /*
         * Clearing the flag re-arms the fast window: the AP simply stays up
         * until 60 s after the STA gets an IP.
         */
        s_ap_fallback = false;
        start_driver(WIFI_MODE_APSTA);
    } else {
        start_driver(WIFI_MODE_STA);
    }
    esp_wifi_disconnect();
    write_sta_config();
    begin_sta_window();
}

static void handle_got_ip(void)
{
    esp_netif_ip_info_t ip;
    state_lock();
    ip = s_sta_ip;
    state_unlock();

    /* Cache the SSID we actually associated with. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        state_lock();
        memcpy(s_ssid, ap.ssid, sizeof(ap.ssid));
        s_ssid[sizeof(s_ssid) - 1] = '\0';
        state_unlock();
    }

    s_attempt = 0;
    timer_stop(s_retry_timer);
    timer_stop(s_fallback_timer);

    const char *host = (s_cfg.hostname[0] != '\0') ? s_cfg.hostname : "netdash";
    ESP_LOGI(TAG, "NetDash STA IP: " IPSTR "  http://%s.local", IP2STR(&ip.ip), host);
    ESP_LOGI(TAG, "gateway " IPSTR " netmask " IPSTR, IP2STR(&ip.gw), IP2STR(&ip.netmask));

    apply_timezone();
    sntp_start();
    mdns_start();

    post_netdash_wifi_event(NETDASH_EVENT_WIFI_STA_GOT_IP, ntohl(ip.ip.addr));

    if (driver_has_ap()) {
        ESP_LOGI(TAG, "STA is up, stopping the setup AP in %d s", AP_LINGER_MS / 1000);
        timer_restart(s_linger_timer, AP_LINGER_MS);
    }
}

static void handle_disconnected(uint32_t reason)
{
    bool had_ip;
    state_lock();
    had_ip   = s_has_ip;
    s_has_ip = false;
    memset(&s_sta_ip, 0, sizeof(s_sta_ip));
    s_ssid[0] = '\0';
    state_unlock();

    if (!s_sta_enabled) {
        return;                     /* deliberate disconnect / AP-only mode */
    }

    if (had_ip) {
        ESP_LOGW(TAG, "STA disconnected (reason %u), link lost", (unsigned)reason);
        post_netdash_wifi_event(NETDASH_EVENT_WIFI_STA_LOST, 0);
    } else {
        ESP_LOGW(TAG, "STA connect failed (reason %u)", (unsigned)reason);
    }

    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        ESP_LOGW(TAG, "SSID \"%s\" not found", s_cfg.wifi_ssid);
        break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        ESP_LOGW(TAG, "authentication rejected, check the password");
        break;
    default:
        break;
    }

    /* Always go through the timer: never reconnect straight from the event. */
    schedule_retry();
}

static void handle_fallback_timer(void)
{
    bool has_ip;
    state_lock();
    has_ip = s_has_ip;
    state_unlock();

    if (has_ip || !s_sta_enabled) {
        return;
    }
    if (!s_ap_fallback) {
        s_ap_fallback = true;
        ESP_LOGW(TAG, "STA down after %d s, bringing up the setup AP \"%s\"",
                 STA_WINDOW_MS / 1000, s_ap_ssid);
        ensure_ap_up();
    }
    /* From here the STA retries on the slow cadence. */
    schedule_retry();
}

static void handle_linger_timer(void)
{
    bool has_ip;
    state_lock();
    has_ip = s_has_ip;
    state_unlock();

    if (!has_ip || !s_sta_enabled || !driver_has_ap()) {
        return;
    }
    ESP_LOGI(TAG, "stopping the setup AP, STA is stable");
    s_ap_fallback = false;
    set_mode(WIFI_MODE_STA);
}

static void handle_ap_started(void)
{
    if (s_ap_netif != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            state_lock();
            s_ap_ip       = ip;
            s_ap_ip_valid = true;
            state_unlock();
            ESP_LOGI(TAG, "AP \"%s\" up on " IPSTR " (channel %d)",
                     s_ap_ssid, IP2STR(&ip.ip), AP_CHANNEL);
        }
    }
    refresh_mode_snapshot();
    mdns_start();
    post_netdash_ap_event(NETDASH_EVENT_WIFI_AP_STARTED);
}

static void handle_ap_stopped(void)
{
    state_lock();
    s_ap_ip_valid = false;
    memset(&s_ap_ip, 0, sizeof(s_ap_ip));
    state_unlock();
    refresh_mode_snapshot();
    ESP_LOGI(TAG, "AP stopped");
    post_netdash_ap_event(NETDASH_EVENT_WIFI_AP_STOPPED);
}

static void wifi_mgr_task(void *arg)
{
    (void)arg;
    wifi_msg_t msg;

    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch ((msg_id_t)msg.id) {
        case MSG_START:
        case MSG_APPLY:
            handle_apply();
            break;

        case MSG_STA_STARTED:
            s_sta_iface_up = true;
            refresh_mode_snapshot();
            try_connect();
            break;

        case MSG_STA_STOPPED:
            s_sta_iface_up = false;
            refresh_mode_snapshot();
            break;

        case MSG_STA_CONNECTED:
            ESP_LOGI(TAG, "associated, waiting for DHCP");
            break;

        case MSG_STA_DISCONNECTED:
            handle_disconnected(msg.arg);
            break;

        case MSG_STA_GOT_IP:
            handle_got_ip();
            break;

        case MSG_STA_LOST_IP:
            ESP_LOGW(TAG, "STA lost its IP");
            post_netdash_wifi_event(NETDASH_EVENT_WIFI_STA_LOST, 0);
            break;

        case MSG_AP_STARTED:
            handle_ap_started();
            break;

        case MSG_AP_STOPPED:
            handle_ap_stopped();
            break;

        case MSG_RETRY_TIMER:
            try_connect();
            break;

        case MSG_FALLBACK_TIMER:
            handle_fallback_timer();
            break;

        case MSG_AP_LINGER_TIMER:
            handle_linger_timer();
            break;

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t wifi_mgr_init(void)
{
    esp_err_t err;

    if (s_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_read_mac(s_base_mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed: %s", esp_err_to_name(err));
        memset(s_base_mac, 0, sizeof(s_base_mac));
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "NetDash-%02X%02X", s_base_mac[4], s_base_mac[5]);

    s_state_lock = xSemaphoreCreateMutex();
    s_scan_lock  = xSemaphoreCreateMutex();
    s_queue      = xQueueCreate(QUEUE_LEN, sizeof(wifi_msg_t));
    if (s_state_lock == NULL || s_scan_lock == NULL || s_queue == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(TAG, "netif creation failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* NetDash keeps the credentials in its own NVS blob. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_storage failed: %s", esp_err_to_name(err));
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb, .name = "wifi_retry"};
    const esp_timer_create_args_t fallback_args = {
        .callback = fallback_timer_cb, .name = "wifi_fallback"};
    const esp_timer_create_args_t linger_args = {
        .callback = linger_timer_cb, .name = "wifi_ap_off"};
    if (esp_timer_create(&retry_args, &s_retry_timer) != ESP_OK ||
        esp_timer_create(&fallback_args, &s_fallback_timer) != ESP_OK ||
        esp_timer_create(&linger_args, &s_linger_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer creation failed");
        return ESP_FAIL;
    }

    /* Hostname must be set before the DHCP client runs. */
    settings_get(&s_cfg);
    apply_hostname();
    apply_timezone();
    s_first_apply = true;

    if (xTaskCreate(wifi_mgr_task, "wifi_mgr", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return ESP_ERR_NO_MEM;
    }

    post_msg(MSG_START, 0);
    ESP_LOGI(TAG, "init done (AP SSID %s)", s_ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_apply_settings(void)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Safe from httpd: the worker task does the driver work. */
    post_msg(MSG_APPLY, 0);
    return ESP_OK;
}

esp_err_t wifi_mgr_get_ip_info(esp_netif_ip_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;

    state_lock();
    if (s_has_ip) {
        *out = s_sta_ip;
        err  = ESP_OK;
    } else if (s_ap_ip_valid) {
        *out = s_ap_ip;
        err  = ESP_OK;
    } else {
        memset(out, 0, sizeof(*out));
    }
    state_unlock();
    return err;
}

wifi_mgr_mode_t wifi_mgr_get_mode(void)
{
    state_lock();
    wifi_mgr_mode_t mode = s_mode;
    state_unlock();
    return mode;
}

int8_t wifi_mgr_get_rssi(void)
{
    bool has_ip;
    state_lock();
    has_ip = s_has_ip;
    state_unlock();
    if (!has_ip) {
        return 0;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

const char *wifi_mgr_get_ssid(void)
{
    return s_ssid;
}

const char *wifi_mgr_get_ap_ssid(void)
{
    return s_ap_ssid;
}

esp_err_t wifi_mgr_get_mac(uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(mac, s_base_mac, 6);
    return ESP_OK;
}

uint32_t wifi_mgr_get_gateway(void)
{
    uint32_t gw = 0;
    state_lock();
    if (s_has_ip) {
        gw = ntohl(s_sta_ip.gw.addr);
    }
    state_unlock();
    return gw;
}

/* ------------------------------------------------------------------------- */
/* Scan                                                                       */
/* ------------------------------------------------------------------------- */

static int record_cmp(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;
    if (ra->rssi > rb->rssi) {
        return -1;
    }
    if (ra->rssi < rb->rssi) {
        return 1;
    }
    return 0;
}

int wifi_mgr_scan(wifi_mgr_ap_record_t *list, size_t max)
{
    if (list == NULL || max == 0) {
        return -1;
    }
    if (!s_wifi_started || s_scan_lock == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGW(TAG, "scan already running");
        return -1;
    }

    int         out_n         = 0;
    wifi_mode_t restore_mode  = WIFI_MODE_NULL;
    bool        mode_switched = false;

    /* Scanning needs a STA interface; in AP-only mode borrow one temporarily. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK) {
            restore_mode  = WIFI_MODE_AP;
            mode_switched = true;
            refresh_mode_snapshot();
            /* Give the STA interface a moment to come up. */
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            ESP_LOGE(TAG, "cannot scan in AP-only mode");
            xSemaphoreGive(s_scan_lock);
            return -1;
        }
    }

    wifi_scan_config_t scan_cfg = {0};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        out_n = -1;
        goto done;
    }

    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK || found == 0) {
        ESP_LOGI(TAG, "scan found no APs");
        out_n = 0;
        goto done;
    }
    if (found > SCAN_MAX_RECORDS) {
        found = SCAN_MAX_RECORDS;
    }

    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        ESP_LOGE(TAG, "scan: out of memory");
        esp_wifi_clear_ap_list();
        out_n = -1;
        goto done;
    }

    uint16_t n = found;
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan records failed: %s", esp_err_to_name(err));
        free(recs);
        out_n = -1;
        goto done;
    }

    qsort(recs, n, sizeof(wifi_ap_record_t), record_cmp);

    for (uint16_t i = 0; i < n && (size_t)out_n < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;                                   /* hidden / broken */
        }
        bool dup = false;
        for (int j = 0; j < out_n; j++) {
            if (strncmp(list[j].ssid, ssid, sizeof(list[j].ssid) - 1) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;                                   /* mesh: keep the best */
        }
        memcpy(list[out_n].ssid, recs[i].ssid, sizeof(recs[i].ssid));
        list[out_n].ssid[sizeof(list[out_n].ssid) - 1] = '\0';
        list[out_n].rssi     = recs[i].rssi;
        list[out_n].authmode = (uint8_t)recs[i].authmode;
        list[out_n].channel  = recs[i].primary;
        out_n++;
    }

    free(recs);
    ESP_LOGI(TAG, "scan: %u APs, %d unique SSIDs returned", (unsigned)n, out_n);

done:
    if (mode_switched) {
        esp_wifi_set_mode(restore_mode);
        refresh_mode_snapshot();
    }
    xSemaphoreGive(s_scan_lock);
    return out_n;
}
