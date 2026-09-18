/*
 * NetBIOS name service discovery: node-status query (UDP 137) for hosts that
 * are still unnamed (NETDASH_NAME_SRC_NBNS).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t disc_nbns_init(void);

/* Queries every still-unnamed online device once. Blocking. */
esp_err_t disc_nbns_run_once(void);

#ifdef __cplusplus
}
#endif
