/*
 * Device classification: vendor, advertised services and hostname keywords to
 * a netdash_type_t. The user override always wins and is applied by the
 * caller, not here.
 */
#pragma once

#include <stdint.h>

#include "device_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns the best guess for dev. gateway_ip is the default gateway in host
 * byte order (0 when unknown) and is what distinguishes a router from a mesh
 * node. Pure function: it does not touch the database.
 */
netdash_type_t classify_device(const netdash_device_t *dev, uint32_t gateway_ip);

/* Stable lowercase name used by the REST API, e.g. "mesh_node". */
const char *netdash_type_name(netdash_type_t type);

/* Inverse of netdash_type_name(); NETDASH_TYPE_UNKNOWN when unrecognised. */
netdash_type_t netdash_type_from_name(const char *name);

/* Stable lowercase name for one NETDASH_SVC_ bit index, e.g. "airplay". */
const char *netdash_service_name(int bit_index);

#ifdef __cplusplus
}
#endif
