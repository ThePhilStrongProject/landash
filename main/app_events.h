/*
 * NetDash application event base.
 *
 * Every module posts to the default event loop with base NETDASH_EVENT so the
 * display, the events log and the HTTP server can react without knowing about
 * each other.
 */
#pragma once

#include <stdint.h>

#include "esp_event.h"

#include "device_db.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(NETDASH_EVENT);

typedef enum {
    /* Wi-Fi manager. Data: netdash_wifi_event_t (NULL for the AP events). */
    NETDASH_EVENT_WIFI_STA_GOT_IP = 0,
    NETDASH_EVENT_WIFI_STA_LOST,
    NETDASH_EVENT_WIFI_AP_STARTED,
    NETDASH_EVENT_WIFI_AP_STOPPED,

    /* Device database. Data: netdash_device_t, copied by value. */
    NETDASH_EVENT_DEVICE_NEW,
    NETDASH_EVENT_DEVICE_ONLINE,
    NETDASH_EVENT_DEVICE_OFFLINE,
    NETDASH_EVENT_DEVICE_IP_CHANGED,

    /* Scanner. Data: netdash_scan_progress_t. */
    NETDASH_EVENT_SCAN_STARTED,
    NETDASH_EVENT_SCAN_PROGRESS,
    NETDASH_EVENT_SCAN_DONE,

    /* SNTP. Data: none. */
    NETDASH_EVENT_TIME_SYNCED,
} netdash_event_id_t;

/* Data for NETDASH_EVENT_SCAN_ events. */
typedef struct {
    uint16_t done;
    uint16_t total;
} netdash_scan_progress_t;

/* Data for NETDASH_EVENT_WIFI_ events. */
typedef struct {
    uint32_t ip;            /* host byte order, 0 when not applicable */
    char     ssid[33];
    int8_t   rssi;
} netdash_wifi_event_t;

#ifdef __cplusplus
}
#endif
