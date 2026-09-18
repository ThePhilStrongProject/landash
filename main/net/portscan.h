/*
 * Background TCP port scanner.
 *
 * Works in tiers, and finishes a tier across every device before starting the
 * next one, so the useful results arrive early:
 *
 *   1  a curated list of common ports          minutes for a home network
 *   2  every port from 1 to 1024               under an hour
 *   3  every remaining port, 1025 to 65535     days
 *
 * The probe rate is a global budget shared by all devices, not a per-device
 * rate, so adding devices makes a pass longer rather than making the scan
 * noisier. Results and per-device tier progress survive a reboot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     enabled;
    bool     running;       /* a probe pass is in flight right now          */
    uint8_t  tier;          /* tier being worked on, 1..3                   */
    uint8_t  max_tier;      /* highest tier this configuration will reach   */
    uint16_t rate;          /* probes per second                            */
    uint16_t device_index;  /* position in the device list for this tier    */
    uint16_t device_count;  /* devices in scope for this tier               */
    uint32_t cursor;        /* index within the current device's tier       */
    uint32_t tier_total;    /* probes per device in this tier               */
    uint32_t probes;        /* probes sent since boot                       */
    uint32_t found;         /* open ports found since boot                  */
    int64_t  cycle_started; /* unix seconds the current tier 1 pass began   */
} portscan_status_t;

/* Starts the scanner task. Safe to call before Wi-Fi is up. */
esp_err_t portscan_init(void);

/* Fills out with a snapshot of the scanner state. */
void portscan_get_status(portscan_status_t *out);

/*
 * Forgets every result for mac and puts it at the head of the queue, so the
 * next probes go to that device. Used by POST /api/devices/{mac}/portscan.
 */
esp_err_t portscan_rescan_device(const uint8_t mac[6]);

/* Re-reads settings (enabled / rate / max tier) without a reboot. */
void portscan_settings_changed(void);

#ifdef __cplusplus
}
#endif
