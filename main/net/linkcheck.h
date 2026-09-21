/*
 * Are the services behind the dashboard links actually answering?
 *
 * A device being reachable and the thing you want being up are different
 * questions. A NAS answers ICMP perfectly well while its web UI is dead, and
 * that is exactly the state a dashboard should be telling you about - the
 * device dot was reporting the wrong one.
 *
 * So each link gets its own periodic TCP connect to the port it points at.
 * One probe at a time with a short timeout, a small gap between them, and a
 * pass every NETDASH_LINKCHECK_PERIOD_S: with at most NETDASH_MAX_LINKS links
 * that is well under one connection per second, which is quieter than the
 * background port scan that is already running.
 *
 * Links whose device is offline are skipped rather than probed. There is no
 * point waiting out a timeout to learn what the sweep already knows, and the
 * UI distinguishes the two cases anyway.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How often every link is re-checked. */
#define NETDASH_LINKCHECK_PERIOD_S 60

/* How long a single connect is given before the service is called down. */
#define NETDASH_LINKCHECK_TIMEOUT_MS 2000

typedef enum {
    NETDASH_SVC_UNKNOWN = 0,   /* not probed yet, or the device is offline */
    NETDASH_SVC_UP,            /* the port accepted a connection          */
    NETDASH_SVC_DOWN,          /* refused, or silent until the timeout    */
} netdash_svc_state_t;

/* Starts the checker task. Safe to call before any link exists. */
esp_err_t linkcheck_init(void);

/*
 * The last result for a link. out_checked receives the unix time of that
 * probe, or 0 when it has not been probed. Returns UNKNOWN for an id that has
 * never been seen.
 */
netdash_svc_state_t linkcheck_get(uint16_t link_id, int64_t *out_checked);

/* Forgets a link's result, so a deleted id cannot report a stale state. */
void linkcheck_forget(uint16_t link_id);

/* Re-probes everything on the next tick instead of waiting out the period. */
void linkcheck_now(void);

/* Stable lowercase name for the REST API. */
const char *netdash_svc_state_name(netdash_svc_state_t state);

#ifdef __cplusplus
}
#endif
