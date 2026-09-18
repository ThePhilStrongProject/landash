/*
 * NetDash persistent settings: one blob in NVS namespace "cfg".
 *
 * The struct is copied in and out, never handed out by pointer, so callers do
 * not have to think about the settings lock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     hostname[32];
    char     ap_pass[65];       /* softAP WPA2 password, generated once     */
    uint16_t scan_interval_min;
    uint16_t hosts_per_sec;
    bool     passive_only;
    char     tz[48];            /* POSIX TZ string                          */
    char     ntp_server[64];
    /*
     * Appended in settings blob version 2. New fields are only ever added at
     * the end, so load_locked() can migrate an older blob by copying it over
     * the defaults and leaving the tail at its default value.
     */
    bool     portscan_enabled;
    uint16_t portscan_rate;     /* probes per second, whole device sweep    */
    uint8_t  portscan_max_tier; /* 1 common, 2 well-known, 3 every port     */
} netdash_settings_t;

/* Loads (or creates) the blob and generates ap_pass on first boot. */
esp_err_t settings_init(void);

/* Re-reads the blob from NVS into the cached copy. */
esp_err_t settings_load(void);

/* Writes the cached copy to NVS. */
esp_err_t settings_save(void);

/* Copies the current settings into out. */
void settings_get(netdash_settings_t *out);

/* Replaces the cached settings with in (values are clamped) and persists. */
esp_err_t settings_set(const netdash_settings_t *in);

/* True when a non-empty SSID is stored. */
bool settings_wifi_configured(void);

/* Clears SSID and password and persists (BOOT-hold recovery, factory reset). */
esp_err_t settings_clear_wifi(void);

#ifdef __cplusplus
}
#endif
