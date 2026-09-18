/*
 * mDNS discovery: browses a fixed service list and maps IPv4 addresses to
 * hostnames (NETDASH_NAME_SRC_MDNS) and service bits.
 *
 * This header also declares the plumbing shared by the four discovery
 * sources (disc_mdns, disc_ssdp, disc_rdns, disc_nbns), which lives in
 * disc_mdns.c: the single discovery task and the device-table snapshot every
 * source works from.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the mDNS responder/browser task. */
esp_err_t disc_mdns_init(void);

/* Runs one full browse pass and merges the results into device_db. Blocking. */
esp_err_t disc_mdns_run_once(void);

/* ------------------------------------------------------------------------- */
/* Shared discovery plumbing (internal to the discovery sources)             */
/* ------------------------------------------------------------------------- */

/*
 * One online device, copied out of device_db under its lock so the sources can
 * do network I/O without holding it. hostname itself is not copied: the only
 * thing a source needs is how good the stored name already is, which name_src
 * carries (NETDASH_NAME_SRC_NONE means "no name at all").
 */
typedef struct {
    uint8_t  mac[6];
    uint32_t ip;            /* last known IPv4, host byte order  */
    uint8_t  name_src;      /* netdash_name_src_t of the name     */
} disc_snap_t;

/*
 * Creates the shared discovery task (once) and subscribes it to SCAN_DONE and
 * WIFI_STA_GOT_IP. Every disc_*_init() calls this; further calls are no-ops.
 */
esp_err_t disc_task_ensure(void);

/*
 * Refreshes the shared snapshot from device_db and hands it to the caller.
 * Returns the number of online devices written and sets *out to the array.
 * The caller owns the snapshot until disc_snapshot_release(), which also
 * serialises the four sources against each other.
 */
size_t disc_snapshot_acquire(const disc_snap_t **out);

void disc_snapshot_release(void);

/* Linear lookup by IPv4 (host byte order). NULL when no online device has it. */
const disc_snap_t *disc_snapshot_find_ip(const disc_snap_t *snap, size_t count, uint32_t ip);

#ifdef __cplusplus
}
#endif
