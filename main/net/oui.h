/*
 * IEEE OUI lookup over a sorted, generated table (see tools/gen_oui.py).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns a short vendor name for the first three bytes of mac, or NULL when
 * the prefix is not in the table. The string is in flash and is never freed.
 * Locally administered addresses (randomised phone MACs) return NULL.
 */
const char *oui_lookup(const uint8_t mac[6]);

/* Number of prefixes in the compiled-in table. */
size_t oui_table_size(void);

#ifdef __cplusplus
}
#endif
