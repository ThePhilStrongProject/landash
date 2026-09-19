/*
 * WAN health: is the internet reachable, and is it the link or DNS that broke?
 *
 * Two probes on the configured interval, both cheap enough to run forever:
 *
 *   ICMP echo to a fixed address (1.1.1.1 by default). Proves the route out
 *   works without depending on name resolution.
 *
 *   A name lookup (example.com by default). Proves DNS answers.
 *
 * Splitting them is the whole point. "The internet is down" and "DNS is down"
 * look identical from a browser and want completely different fixes, and
 * telling them apart is exactly the thing that is hard to remember how to do
 * at the moment you need to.
 *
 * One caveat worth knowing: lwIP caches DNS answers for their TTL, so a lookup
 * that succeeds may have been answered from the cache rather than the wire.
 * A real resolver failure shows up within a TTL, not instantly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETDASH_WAN_UNKNOWN = 0,   /* not checked yet, or switched off        */
    NETDASH_WAN_UP,            /* both probes answered                    */
    NETDASH_WAN_DEGRADED,      /* one answered, the other did not         */
    NETDASH_WAN_DOWN,          /* neither answered                        */
} netdash_wan_state_t;

typedef struct {
    uint8_t  state;            /* netdash_wan_state_t                     */
    bool     icmp_ok;
    bool     dns_ok;
    int16_t  rtt_ms;           /* last successful echo, -1 when none      */
    int64_t  last_check;       /* unix seconds, 0 before the first check  */
    int64_t  changed_at;       /* unix seconds the state last changed     */
    uint32_t checks;
    uint32_t failures;         /* checks that were not fully UP           */
} netdash_wan_t;

/* Starts the monitor task. Safe to call before Wi-Fi is associated. */
esp_err_t wan_init(void);

/* Copies the latest result. Always safe, even before the first check. */
void wan_get(netdash_wan_t *out);

/* Runs a check on the next tick rather than waiting out the interval. */
void wan_check_now(void);

/* Stable lowercase name for the REST API, e.g. "degraded". */
const char *netdash_wan_state_name(netdash_wan_state_t state);

#ifdef __cplusplus
}
#endif
