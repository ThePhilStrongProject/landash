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

#define NETDASH_MAX_LINKS   48
#define NETDASH_LINK_LABEL  32
#define NETDASH_LINK_ICON   16
#define NETDASH_MAX_GROUPS  8
#define NETDASH_GROUP_NAME  24

typedef enum {
    NETDASH_SCHEME_HTTP = 0,
    NETDASH_SCHEME_HTTPS,
} netdash_scheme_t;

typedef struct {
    uint16_t id;                        /* stable, never reused within a boot */
    uint8_t  mac[6];
    uint16_t port;
    uint8_t  scheme;                    /* netdash_scheme_t                   */
    uint8_t  group;                     /* group id, 0 = ungrouped            */
    /*
     * An icon override, as a bare sprite id from the web UI's icon sheet. An
     * empty string means "work it out from the service on this port", which
     * the UI does client-side - keeping the service-to-icon map in the page
     * means adding an icon never needs new firmware.
     */
    char     icon[NETDASH_LINK_ICON];
    char     label[NETDASH_LINK_LABEL];
} netdash_link_t;

/*
 * A heading on the dashboard. Groups are ordered by their position in the
 * table; links are ordered within a group by their position in the link list.
 */
typedef struct {
    uint8_t id;                         /* 1..255, never 0 and never reused   */
    char    name[NETDASH_GROUP_NAME];
} netdash_group_t;

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

/*
 * Updates the fields of an existing link. Pass NULL for label or icon,
 * UINT16_MAX for port, or a negative scheme / group to leave that field alone.
 * An empty icon string clears the override; group 0 moves the link out of
 * whatever group it was in.
 */
esp_err_t links_update(uint16_t id, const char *label, uint16_t port, int scheme,
                       const char *icon, int group);

esp_err_t links_remove(uint16_t id);

/*
 * Reorders the list to exactly the given sequence of ids. Any link whose id is
 * absent from the sequence is removed, which makes this a bulk delete as well.
 * Returns ESP_ERR_INVALID_ARG if any id is unknown, leaving the list untouched.
 */
esp_err_t links_reorder(const uint16_t *ids, size_t count);

/* ------------------------------------------------------------------------- */
/* Groups                                                                    */
/* ------------------------------------------------------------------------- */

size_t links_group_count(void);

/* Copies the group at index (0 .. links_group_count()-1). False past the end. */
bool links_group_get_at(size_t index, netdash_group_t *out);

/*
 * Appends a group. Returns ESP_ERR_NO_MEM when there are already
 * NETDASH_MAX_GROUPS of them, ESP_ERR_INVALID_ARG on an empty name.
 */
esp_err_t links_group_add(const char *name, uint8_t *out_id);

esp_err_t links_group_rename(uint8_t id, const char *name);

/* Removes the group. Links that were in it become ungrouped, never deleted. */
esp_err_t links_group_remove(uint8_t id);

/*
 * Reorders the groups to exactly the given sequence of ids, which must be a
 * permutation of the existing ones. Leaves the table untouched and returns
 * ESP_ERR_INVALID_ARG if it is not.
 */
esp_err_t links_group_reorder(const uint8_t *ids, size_t count);

/* ------------------------------------------------------------------------- */

/* The scheme a port implies, for when the caller did not specify one. */
netdash_scheme_t links_default_scheme(uint16_t port);

const char *netdash_scheme_name(netdash_scheme_t scheme);

#ifdef __cplusplus
}
#endif
