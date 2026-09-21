/*
 * NetDash persistent settings.
 *
 * One versioned blob in NVS namespace "cfg", key "blob". The cached copy is
 * guarded by a mutex; callers only ever see copies.
 */
#include "settings.h"

#include "notify.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define SETTINGS_NS      "cfg"
#define SETTINGS_KEY     "blob"
#define SETTINGS_VER_KEY "ver"
#define SETTINGS_VERSION 6

#define AP_PASS_LEN 8

static netdash_settings_t s_cfg;
static SemaphoreHandle_t  s_lock;
static bool               s_ready;

/* ------------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* Copies src into dst[cap] and always NUL-terminates. */
static void str_set(char *dst, size_t cap, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/*
 * Generates a readable 8-character WPA2 password from the base MAC. The
 * alphabet avoids characters that are easy to misread off a 1.14" LCD.
 */
static void gen_ap_pass(char *out, size_t cap)
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    const size_t      n_alpha    = sizeof(alphabet) - 1;
    uint8_t           mac[6]     = {0};

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed, AP password will be weak");
    }

    /* Mix the MAC into a 32-bit state, then walk it through the alphabet. */
    uint32_t state = 0x811c9dc5u;
    for (int i = 0; i < 6; i++) {
        state = (state ^ mac[i]) * 16777619u;
    }

    size_t len = (cap - 1 < AP_PASS_LEN) ? cap - 1 : AP_PASS_LEN;
    for (size_t i = 0; i < len; i++) {
        out[i] = alphabet[state % n_alpha];
        state  = state * 1103515245u + 12345u;
    }
    out[len] = '\0';
}

static void apply_defaults(netdash_settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    str_set(cfg->hostname, sizeof(cfg->hostname), CONFIG_NETDASH_HOSTNAME);
    str_set(cfg->tz, sizeof(cfg->tz), "UTC0");
    str_set(cfg->ntp_server, sizeof(cfg->ntp_server), "pool.ntp.org");
    cfg->scan_interval_min = CONFIG_NETDASH_SCAN_INTERVAL_MIN;
    cfg->hosts_per_sec     = CONFIG_NETDASH_SCAN_HOSTS_PER_SEC;
    cfg->passive_only      = false;
    cfg->portscan_enabled  = CONFIG_NETDASH_PORTSCAN_ENABLED;
    cfg->portscan_rate     = CONFIG_NETDASH_PORTSCAN_RATE;
    cfg->portscan_max_tier = CONFIG_NETDASH_PORTSCAN_MAX_TIER;

    cfg->notif_mask           = NETDASH_NOTIF_DEFAULT_MASK;
    cfg->wan_enabled          = true;
    cfg->wan_interval_s       = 60;
    cfg->portscan_rescan_days = 7;
    str_set(cfg->wan_ping_host, sizeof(cfg->wan_ping_host), "1.1.1.1");
    str_set(cfg->wan_dns_probe, sizeof(cfg->wan_dns_probe), "example.com");
    str_set(cfg->theme, sizeof(cfg->theme), "dark");
    str_set(cfg->detail, sizeof(cfg->detail), "comfort");
    cfg->ota_enabled    = true;
    cfg->ota_auto       = true;
    cfg->ota_interval_h = 12;
}

static void clamp(netdash_settings_t *cfg)
{
    /* Defend against a hand-edited blob or a bad PUT /api/settings. */
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1]   = '\0';
    cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1]   = '\0';
    cfg->hostname[sizeof(cfg->hostname) - 1]     = '\0';
    cfg->ap_pass[sizeof(cfg->ap_pass) - 1]       = '\0';
    cfg->tz[sizeof(cfg->tz) - 1]                 = '\0';
    cfg->ntp_server[sizeof(cfg->ntp_server) - 1] = '\0';

    if (cfg->hostname[0] == '\0') {
        str_set(cfg->hostname, sizeof(cfg->hostname), CONFIG_NETDASH_HOSTNAME);
    }
    if (cfg->ntp_server[0] == '\0') {
        str_set(cfg->ntp_server, sizeof(cfg->ntp_server), "pool.ntp.org");
    }
    if (cfg->tz[0] == '\0') {
        str_set(cfg->tz, sizeof(cfg->tz), "UTC0");
    }
    if (cfg->scan_interval_min < 1) {
        cfg->scan_interval_min = 1;
    } else if (cfg->scan_interval_min > 1440) {
        cfg->scan_interval_min = 1440;
    }
    if (cfg->hosts_per_sec < 1) {
        cfg->hosts_per_sec = 1;
    } else if (cfg->hosts_per_sec > 64) {
        cfg->hosts_per_sec = 64;
    }
    if (cfg->portscan_rate < 1) {
        cfg->portscan_rate = 1;
    } else if (cfg->portscan_rate > 200) {
        cfg->portscan_rate = 200;
    }
    if (cfg->portscan_max_tier < 1) {
        cfg->portscan_max_tier = 1;
    } else if (cfg->portscan_max_tier > 3) {
        cfg->portscan_max_tier = 3;
    }
    cfg->wan_ping_host[sizeof(cfg->wan_ping_host) - 1] = '\0';
    cfg->wan_dns_probe[sizeof(cfg->wan_dns_probe) - 1] = '\0';
    if (cfg->wan_ping_host[0] == '\0') {
        str_set(cfg->wan_ping_host, sizeof(cfg->wan_ping_host), "1.1.1.1");
    }
    if (cfg->wan_dns_probe[0] == '\0') {
        str_set(cfg->wan_dns_probe, sizeof(cfg->wan_dns_probe), "example.com");
    }
    /* Under 15 s the checks would be their own kind of network noise. */
    if (cfg->wan_interval_s < 15) {
        cfg->wan_interval_s = 15;
    } else if (cfg->wan_interval_s > 3600) {
        cfg->wan_interval_s = 3600;
    }
    if (cfg->portscan_rescan_days > 365) {
        cfg->portscan_rescan_days = 365;
    }
    cfg->notif_mask &= NETDASH_NOTIF_ALL_MASK;

    cfg->theme[sizeof(cfg->theme) - 1]   = '\0';
    cfg->detail[sizeof(cfg->detail) - 1] = '\0';
    if (cfg->theme[0] == '\0') {
        str_set(cfg->theme, sizeof(cfg->theme), "dark");
    }
    if (cfg->detail[0] == '\0') {
        str_set(cfg->detail, sizeof(cfg->detail), "comfort");
    }
    if (cfg->ota_interval_h < 1) {
        cfg->ota_interval_h = 1;
    } else if (cfg->ota_interval_h > 168) {
        cfg->ota_interval_h = 168;
    }

    /* WPA2 needs 8..63 characters; anything shorter would fail to start. */
    if (strlen(cfg->ap_pass) < 8) {
        gen_ap_pass(cfg->ap_pass, sizeof(cfg->ap_pass));
    }
}

/* Writes s_cfg to NVS. Caller holds the lock. */
static esp_err_t save_locked(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, SETTINGS_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_set_u16(h, SETTINGS_VER_KEY, SETTINGS_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

/*
 * Restores one field from the defaults when the stored blob was too short to
 * contain all of it. Comparing against the end of the field rather than its
 * start covers both a field the old layout never had and a field the old
 * layout ended part-way through (or ended in padding just before).
 *
 * Only valid inside load_locked(), which defines `stored` and `fresh`.
 */
#define MIGRATE_FIELD(f)                                                       \
    do {                                                                       \
        if (stored < offsetof(netdash_settings_t, f) + sizeof(s_cfg.f)) {      \
            memcpy(&s_cfg.f, &fresh.f, sizeof(s_cfg.f));                       \
        }                                                                      \
    } while (0)

/* Reads NVS into s_cfg, falling back to defaults. Caller holds the lock. */
static esp_err_t load_locked(bool *out_dirty)
{
    bool dirty = false;

    apply_defaults(&s_cfg);

    nvs_handle_t h;
    esp_err_t    err = nvs_open(SETTINGS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        dirty = true;
        err   = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    } else {
        uint16_t ver    = 0;
        size_t   stored = 0;

        if (nvs_get_u16(h, SETTINGS_VER_KEY, &ver) != ESP_OK) {
            ver = 0;
        }

        /* Ask for the stored size first so an older, shorter blob can be
           migrated rather than thrown away along with the Wi-Fi password. */
        esp_err_t rerr = nvs_get_blob(h, SETTINGS_KEY, NULL, &stored);

        if (rerr == ESP_OK && ver == SETTINGS_VERSION && stored == sizeof(s_cfg)) {
            size_t size = sizeof(s_cfg);
            rerr = nvs_get_blob(h, SETTINGS_KEY, &s_cfg, &size);
            if (rerr != ESP_OK) {
                ESP_LOGW(TAG, "settings read failed (%s), resetting", esp_err_to_name(rerr));
                apply_defaults(&s_cfg);
                dirty = true;
            }
        } else if (rerr == ESP_OK && stored > 0 && stored < sizeof(s_cfg)) {
            /*
             * An older, shorter layout. Fields are only ever appended, so the
             * stored bytes line up with the head of the current struct: copy
             * them over the defaults and let the new tail keep its default.
             */
            uint8_t *scratch = calloc(1, stored);
            size_t   size    = stored;

            if (scratch != NULL && nvs_get_blob(h, SETTINGS_KEY, scratch, &size) == ESP_OK &&
                size == stored) {
                memcpy(&s_cfg, scratch, stored);

                /*
                 * The old layout may have ended in a padding byte, which the
                 * copy above would have dropped onto the first appended field.
                 * Re-apply the default of every field added after the v6
                 * baseline here, one MIGRATE_FIELD() each, so none of them
                 * inherits a stale pad byte. There are none yet.
                 */
                netdash_settings_t fresh;
                apply_defaults(&fresh);
                (void)fresh;

                ESP_LOGW(TAG, "migrated settings from v%u (%u bytes) to v%u (%u bytes)",
                         (unsigned)ver, (unsigned)stored,
                         (unsigned)SETTINGS_VERSION, (unsigned)sizeof(s_cfg));
            } else {
                ESP_LOGW(TAG, "settings migration failed, resetting");
                apply_defaults(&s_cfg);
            }
            free(scratch);
            dirty = true;   /* rewrite in the current layout */
        } else if (rerr == ESP_OK && stored > sizeof(s_cfg) && stored <= 4096) {
            /*
             * A longer layout: written by newer firmware - which is what a
             * rollback after a failed update leaves behind - or one that has
             * since dropped a field off the end. Fields only ever append, so
             * the head is exactly this layout. Keep it rather than throwing
             * the Wi-Fi password away at the worst possible moment, and
             * rewrite it so a dropped field does not linger in flash.
             */
            uint8_t *scratch = calloc(1, stored);
            size_t   size    = stored;

            if (scratch != NULL && nvs_get_blob(h, SETTINGS_KEY, scratch, &size) == ESP_OK &&
                size == stored) {
                memcpy(&s_cfg, scratch, sizeof(s_cfg));
                ESP_LOGW(TAG, "settings blob is v%u (%u bytes); kept the first %u bytes as v%u",
                         (unsigned)ver, (unsigned)stored, (unsigned)sizeof(s_cfg),
                         (unsigned)SETTINGS_VERSION);
            } else {
                ESP_LOGW(TAG, "settings read failed, resetting");
                apply_defaults(&s_cfg);
            }
            free(scratch);
            dirty = true;
        } else {
            ESP_LOGW(TAG, "stored settings unusable (err=%s size=%u ver=%u), resetting",
                     esp_err_to_name(rerr), (unsigned)stored, (unsigned)ver);
            apply_defaults(&s_cfg);
            dirty = true;
        }
        nvs_close(h);
    }

    clamp(&s_cfg);

    if (out_dirty) {
        *out_dirty = dirty;
    }
    return err;
}

/* ------------------------------------------------------------------------- */

esp_err_t settings_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock();
    bool      dirty = false;
    esp_err_t err   = load_locked(&dirty);
    if (err == ESP_OK) {
        s_ready = true;
        /* First boot: persist the generated AP password so it is stable. */
        if (dirty) {
            (void)save_locked();
        }
    }
    ESP_LOGI(TAG, "hostname=%s ap_pass=%s interval=%umin rate=%u/s passive=%d",
             s_cfg.hostname, s_cfg.ap_pass,
             (unsigned)s_cfg.scan_interval_min, (unsigned)s_cfg.hosts_per_sec,
             (int)s_cfg.passive_only);
    unlock();

    return err;
}

esp_err_t settings_load(void)
{
    lock();
    esp_err_t err = load_locked(NULL);
    unlock();
    return err;
}

esp_err_t settings_save(void)
{
    lock();
    esp_err_t err = save_locked();
    unlock();
    return err;
}

void settings_get(netdash_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    memcpy(out, &s_cfg, sizeof(*out));
    unlock();
}

esp_err_t settings_set(const netdash_settings_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    memcpy(&s_cfg, in, sizeof(s_cfg));
    clamp(&s_cfg);
    esp_err_t err = save_locked();
    unlock();
    return err;
}

bool settings_wifi_configured(void)
{
    lock();
    bool configured = s_cfg.wifi_ssid[0] != '\0';
    unlock();
    return configured;
}

esp_err_t settings_clear_wifi(void)
{
    lock();
    memset(s_cfg.wifi_ssid, 0, sizeof(s_cfg.wifi_ssid));
    memset(s_cfg.wifi_pass, 0, sizeof(s_cfg.wifi_pass));
    esp_err_t err = save_locked();
    unlock();
    ESP_LOGW(TAG, "Wi-Fi credentials cleared");
    return err;
}
