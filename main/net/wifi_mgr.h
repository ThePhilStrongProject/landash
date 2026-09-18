/*
 * NetDash Wi-Fi manager: STA with backoff, APSTA fallback, hostname, SNTP and
 * the mDNS advertisement.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_MODE_OFF = 0,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_AP,
    WIFI_MGR_MODE_APSTA,
} wifi_mgr_mode_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode;       /* wifi_auth_mode_t */
    uint8_t channel;
} wifi_mgr_ap_record_t;

/*
 * Brings up netifs and the Wi-Fi driver and starts the connect state machine.
 * Requires settings_init() and the default event loop.
 */
esp_err_t wifi_mgr_init(void);

/* Current STA (or AP, when AP-only) IP info. ESP_ERR_INVALID_STATE if none. */
esp_err_t wifi_mgr_get_ip_info(esp_netif_ip_info_t *out);

wifi_mgr_mode_t wifi_mgr_get_mode(void);

/* STA RSSI in dBm, 0 when not associated. */
int8_t wifi_mgr_get_rssi(void);

/* Re-reads settings and restarts the connect state machine. */
esp_err_t wifi_mgr_apply_settings(void);

/*
 * Blocking active scan. Fills up to max records, strongest first.
 * Returns the number written, or a negative value on error.
 */
int wifi_mgr_scan(wifi_mgr_ap_record_t *list, size_t max);

/* Associated SSID in STA mode, otherwise an empty string. Never NULL. */
const char *wifi_mgr_get_ssid(void);

/* SoftAP SSID ("NetDash-XXXX"). Valid whether or not the AP is up. */
const char *wifi_mgr_get_ap_ssid(void);

/* Base MAC of the dongle. */
esp_err_t wifi_mgr_get_mac(uint8_t mac[6]);

/* Default gateway as a host-order IPv4, 0 when unknown. Used by classify. */
uint32_t wifi_mgr_get_gateway(void);

#ifdef __cplusplus
}
#endif
