/*
 * Reverse-DNS discovery: hand-rolled DNS PTR queries to the gateway, which on
 * a typical consumer router (dnsmasq) answers with the DHCP lease hostname
 * (NETDASH_NAME_SRC_RDNS).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t disc_rdns_init(void);

/* Sends a PTR query for every still-unnamed online device. Blocking. */
esp_err_t disc_rdns_run_once(void);

#ifdef __cplusplus
}
#endif
