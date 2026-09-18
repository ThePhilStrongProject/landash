/*
 * SSDP discovery: M-SEARCH ssdp:all, parses SERVER / LOCATION / USN and
 * optionally fetches the description XML for friendlyName
 * (NETDASH_NAME_SRC_SSDP).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t disc_ssdp_init(void);

/* Sends one M-SEARCH burst, collects replies and merges them. Blocking. */
esp_err_t disc_ssdp_run_once(void);

#ifdef __cplusplus
}
#endif
