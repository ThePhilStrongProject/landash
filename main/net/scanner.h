/*
 * NetDash subnet scanner: paced ICMP echo plus ARP lookup over the connected
 * subnet, with a passive ARP-table snapshot at the end of each sweep.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the scanner task. Sweeps start once Wi-Fi has an IP. */
esp_err_t scanner_start(void);

/* Asks for a sweep as soon as possible. No-op while one is running. */
void scanner_trigger_now(void);

bool scanner_is_running(void);

/* Unix seconds of the last completed sweep, 0 when none yet. */
int64_t scanner_last_sweep_time(void);

/* Progress of the running sweep; both zero when idle. Either pointer may be NULL. */
void scanner_get_progress(uint16_t *done, uint16_t *total);

#ifdef __cplusplus
}
#endif
