/*
 * Dashboard quick links.
 *
 * A link is stored against a device MAC and a port, never an IP, so it keeps
 * working when DHCP hands the device a different address. The address is
 * resolved from the device table each time the list is read.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETDASH_MAX_LINKS   24
#define NETDASH_LINK_LABEL  32

typedef enum {
    NETDASH_SCHEME_HTTP = 0,
    NETDASH_SCHEME_HTTPS,
} netdash_scheme_t;

typedef struct {
    uint16_t id;                        /* stable, never reused within a boot */
    uint8_t  mac[6];
    uint16_t port;
    uint8_t  scheme;                    /* netdash_scheme_t                   */
    char     label[NETDASH_LINK_LABEL];
} netdash_link_t;

/* Loads the stored links from NVS. */
esp_err_t links_init(void);

size_t links_count(void);

/* Copies the link at index (0 .. links_count()-1). False past the end. */
bool links_get_at(size_t index, netdash_link_t *out);

/* Copies the link with the given id. False when it does not exist. */
bool links_get_by_id(uint16_t id, netdash_link_t *out);

/*
 * Appends a link. label may be NULL or empty, in which case the caller is
 * expected to have resolved a sensible default already. Returns
 * ESP_ERR_NO_MEM when the list is full. On success *out_id is the new id.
 */
esp_err_t links_add(const uint8_t mac[6], uint16_t port, netdash_scheme_t scheme,
                    const char *label, uint16_t *out_id);

/* Updates the fields of an existing link. Pass NULL / UINT16_MAX to keep one. */
esp_err_t links_update(uint16_t id, const char *label, uint16_t port, int scheme);

esp_err_t links_remove(uint16_t id);

/*
 * Reorders the list to exactly the given sequence of ids. Any link whose id is
 * absent from the sequence is removed, which makes this a bulk delete as well.
 * Returns ESP_ERR_INVALID_ARG if any id is unknown, leaving the list untouched.
 */
esp_err_t links_reorder(const uint16_t *ids, size_t count);

/* The scheme a port implies, for when the caller did not specify one. */
netdash_scheme_t links_default_scheme(uint16_t port);

const char *netdash_scheme_name(netdash_scheme_t scheme);

#ifdef __cplusplus
}
#endif
