/*
 * Dashboard quick links. See links.h for why a link stores a MAC, not an IP.
 *
 * The whole list is one NVS blob rather than a key per link: order matters, so
 * any change rewrites everything anyway, and a single blob makes reordering
 * and regrouping atomic. Groups live in the same blob for the same reason.
 */
#include "links.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "linkcheck.h"
#include "notes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "links";

#define LINKS_NVS_NS   "links"
#define LINKS_KEY      "list"
#define LINKS_VERSION  2

typedef struct __attribute__((packed)) {
    uint8_t         version;
    uint8_t         count;
    uint16_t        next_id;
    uint8_t         group_count;
    uint8_t         next_group_id;
    netdash_group_t groups[NETDASH_MAX_GROUPS];
    netdash_link_t  items[NETDASH_MAX_LINKS];
} links_blob_t;

/*
 * The version 1 layout, reproduced byte for byte so a dashboard built before
 * groups and icons existed survives the upgrade. netdash_link_t was not packed
 * then either, so this must not be packed now.
 */
typedef struct {
    uint16_t id;
    uint8_t  mac[6];
    uint16_t port;
    uint8_t  scheme;
    char     label[32];
} link_v1_t;

typedef struct __attribute__((packed)) {
    uint8_t   version;
    uint8_t   count;
    uint16_t  next_id;
    link_v1_t items[24];
} links_blob_v1_t;

_Static_assert(sizeof(link_v1_t) == 44, "v1 link layout changed; migration would misread");
_Static_assert(sizeof(links_blob_v1_t) == 1060, "v1 blob layout changed");

static netdash_link_t    s_links[NETDASH_MAX_LINKS];
static size_t            s_count;
static uint16_t          s_next_id = 1;
static netdash_group_t   s_groups[NETDASH_MAX_GROUPS];
static size_t            s_group_count;
static uint8_t           s_next_group_id = 1;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* Caller holds the lock. */
static void save_locked(void)
{
    links_blob_t blob = {
        .version       = LINKS_VERSION,
        .count         = (uint8_t)s_count,
        .next_id       = s_next_id,
        .group_count   = (uint8_t)s_group_count,
        .next_group_id = s_next_group_id,
    };
    memcpy(blob.groups, s_groups, sizeof(blob.groups));
    memcpy(blob.items, s_links, sizeof(blob.items));

    nvs_handle_t h;
    if (nvs_open(LINKS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, links not saved");
        return;
    }
    if (nvs_set_blob(h, LINKS_KEY, &blob, sizeof(blob)) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "nvs_set_blob failed, links not saved");
    }
    nvs_close(h);
}

/* Caller holds the lock. Returns -1 when the id is unknown. */
static int find_locked(uint16_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_links[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* Caller holds the lock. Returns -1 when the id is unknown. */
static int find_group_locked(uint8_t id)
{
    for (size_t i = 0; i < s_group_count; i++) {
        if (s_groups[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* Caller holds the lock. Trims and clamps a group name into place. */
static bool set_group_name(netdash_group_t *g, const char *name)
{
    if (name == NULL) {
        return false;
    }
    while (*name == ' ' || *name == '\t') {
        name++;
    }
    if (*name == '\0') {
        return false;
    }
    memset(g->name, 0, sizeof(g->name));
    strncpy(g->name, name, sizeof(g->name) - 1);

    size_t n = strlen(g->name);
    while (n > 0 && (g->name[n - 1] == ' ' || g->name[n - 1] == '\t')) {
        g->name[--n] = '\0';
    }
    return g->name[0] != '\0';
}

/* Caller holds the lock. Drops references to groups that no longer exist. */
static void reconcile_groups_locked(void)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_links[i].group != 0 && find_group_locked(s_links[i].group) < 0) {
            s_links[i].group = 0;
        }
    }
}

/* Caller holds the lock. Returns true when a v1 blob was converted. */
static bool migrate_v1_locked(nvs_handle_t h, size_t stored)
{
    if (stored != sizeof(links_blob_v1_t)) {
        return false;
    }

    links_blob_v1_t old;
    size_t          size = sizeof(old);
    if (nvs_get_blob(h, LINKS_KEY, &old, &size) != ESP_OK || size != sizeof(old) ||
        old.version != 1) {
        return false;
    }

    s_count = old.count > 24 ? 24 : old.count;
    for (size_t i = 0; i < s_count; i++) {
        netdash_link_t *l = &s_links[i];
        memset(l, 0, sizeof(*l));
        l->id     = old.items[i].id;
        l->port   = old.items[i].port;
        l->scheme = old.items[i].scheme;
        l->group  = 0;           /* everything starts ungrouped */
        l->icon[0] = '\0';       /* and with the derived icon   */
        memcpy(l->mac, old.items[i].mac, 6);
        memcpy(l->label, old.items[i].label, sizeof(l->label) - 1);
        l->label[sizeof(l->label) - 1] = '\0';
    }
    s_next_id       = old.next_id != 0 ? old.next_id : 1;
    s_group_count   = 0;
    s_next_group_id = 1;

    ESP_LOGW(TAG, "migrated %u link(s) from the v1 layout", (unsigned)s_count);
    return true;
}

esp_err_t links_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock();
    s_count         = 0;
    s_next_id       = 1;
    s_group_count   = 0;
    s_next_group_id = 1;
    memset(s_links, 0, sizeof(s_links));
    memset(s_groups, 0, sizeof(s_groups));

    bool migrated = false;

    nvs_handle_t h;
    if (nvs_open(LINKS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t stored = 0;

        /* Ask for the size first so a v1 blob can be recognised rather than
           thrown away along with every link the user had set up. */
        if (nvs_get_blob(h, LINKS_KEY, NULL, &stored) == ESP_OK) {
            if (stored == sizeof(links_blob_t)) {
                /* 3 KB, wanted only for the length of this read: too big for
                   the stack, and not worth holding statically for ever. */
                links_blob_t *blob = calloc(1, sizeof(*blob));
                size_t        size = sizeof(*blob);

                if (blob != NULL && nvs_get_blob(h, LINKS_KEY, blob, &size) == ESP_OK &&
                    size == sizeof(*blob) && blob->version == LINKS_VERSION) {
                    s_count = blob->count > NETDASH_MAX_LINKS ? NETDASH_MAX_LINKS : blob->count;
                    memcpy(s_links, blob->items, sizeof(s_links));
                    s_next_id = blob->next_id != 0 ? blob->next_id : 1;

                    s_group_count = blob->group_count > NETDASH_MAX_GROUPS ? NETDASH_MAX_GROUPS
                                                                           : blob->group_count;
                    memcpy(s_groups, blob->groups, sizeof(s_groups));
                    s_next_group_id = blob->next_group_id != 0 ? blob->next_group_id : 1;
                }
                free(blob);
            } else {
                migrated = migrate_v1_locked(h, stored);
            }
        }
        nvs_close(h);
    }

    /* Defend against a truncated string in a hand-edited blob. */
    for (size_t i = 0; i < s_count; i++) {
        s_links[i].label[NETDASH_LINK_LABEL - 1] = '\0';
        s_links[i].icon[NETDASH_LINK_ICON - 1]   = '\0';
    }
    for (size_t i = 0; i < s_group_count; i++) {
        s_groups[i].name[NETDASH_GROUP_NAME - 1] = '\0';
    }
    reconcile_groups_locked();

    if (migrated) {
        save_locked();
    }
    const size_t loaded = s_count;
    const size_t groups = s_group_count;
    unlock();

    ESP_LOGI(TAG, "loaded %u dashboard link(s) in %u group(s)", (unsigned)loaded,
             (unsigned)groups);
    return ESP_OK;
}

size_t links_count(void)
{
    lock();
    const size_t n = s_count;
    unlock();
    return n;
}

bool links_get_at(size_t index, netdash_link_t *out)
{
    bool ok = false;
    lock();
    if (index < s_count && out != NULL) {
        *out = s_links[index];
        ok   = true;
    }
    unlock();
    return ok;
}

bool links_get_by_id(uint16_t id, netdash_link_t *out)
{
    bool ok = false;
    lock();
    const int idx = find_locked(id);
    if (idx >= 0 && out != NULL) {
        *out = s_links[idx];
        ok   = true;
    }
    unlock();
    return ok;
}

esp_err_t links_add(const uint8_t mac[6], uint16_t port, netdash_scheme_t scheme,
                    const char *label, uint16_t *out_id)
{
    if (mac == NULL || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (s_count >= NETDASH_MAX_LINKS) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    netdash_link_t *l = &s_links[s_count];
    memset(l, 0, sizeof(*l));
    l->id     = s_next_id++;
    l->port   = port;
    l->scheme = (uint8_t)scheme;
    l->group  = 0;
    memcpy(l->mac, mac, 6);
    if (label != NULL) {
        strncpy(l->label, label, sizeof(l->label) - 1);
    }
    s_count++;

    if (out_id != NULL) {
        *out_id = l->id;
    }
    save_locked();
    unlock();
    return ESP_OK;
}

esp_err_t links_update(uint16_t id, const char *label, uint16_t port, int scheme,
                       const char *icon, int group)
{
    lock();
    const int idx = find_locked(id);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (group > 0 && find_group_locked((uint8_t)group) < 0) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }

    if (label != NULL) {
        memset(s_links[idx].label, 0, sizeof(s_links[idx].label));
        strncpy(s_links[idx].label, label, sizeof(s_links[idx].label) - 1);
    }
    if (icon != NULL) {
        memset(s_links[idx].icon, 0, sizeof(s_links[idx].icon));
        strncpy(s_links[idx].icon, icon, sizeof(s_links[idx].icon) - 1);
    }
    if (port != UINT16_MAX && port != 0) {
        s_links[idx].port = port;
    }
    if (scheme >= 0) {
        s_links[idx].scheme = (uint8_t)scheme;
    }
    if (group >= 0) {
        s_links[idx].group = (uint8_t)group;
    }
    save_locked();
    unlock();
    return ESP_OK;
}

esp_err_t links_remove(uint16_t id)
{
    lock();
    const int idx = find_locked(id);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* Order is user-defined, so close the gap rather than swapping the tail. */
    for (size_t i = (size_t)idx; i + 1 < s_count; i++) {
        s_links[i] = s_links[i + 1];
    }
    s_count--;
    memset(&s_links[s_count], 0, sizeof(s_links[s_count]));

    save_locked();
    unlock();

    /* Whatever was written about this link goes with it. */
    link_note_forget(id);
    link_secret_forget(id);
    linkcheck_forget(id);
    return ESP_OK;
}

esp_err_t links_reorder(const uint16_t *ids, size_t count)
{
    if (ids == NULL || count > NETDASH_MAX_LINKS) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();

    /* Build the new list first, so an unknown id leaves the old one intact. */
    static netdash_link_t next[NETDASH_MAX_LINKS];
    for (size_t i = 0; i < count; i++) {
        const int idx = find_locked(ids[i]);
        if (idx < 0) {
            unlock();
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (next[j].id == ids[i]) {
                unlock();
                return ESP_ERR_INVALID_ARG;   /* duplicate id */
            }
        }
        next[i] = s_links[idx];
    }

    /* This doubles as a bulk delete, so collect what is about to disappear
       before the list is replaced. */
    uint16_t dropped[NETDASH_MAX_LINKS];
    size_t   n_dropped = 0;
    for (size_t i = 0; i < s_count; i++) {
        bool kept = false;
        for (size_t j = 0; j < count; j++) {
            if (ids[j] == s_links[i].id) {
                kept = true;
                break;
            }
        }
        if (!kept && n_dropped < NETDASH_MAX_LINKS) {
            dropped[n_dropped++] = s_links[i].id;
        }
    }

    memset(s_links, 0, sizeof(s_links));
    memcpy(s_links, next, count * sizeof(next[0]));
    s_count = count;

    save_locked();
    unlock();

    for (size_t i = 0; i < n_dropped; i++) {
        link_note_forget(dropped[i]);
        link_secret_forget(dropped[i]);
        linkcheck_forget(dropped[i]);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Groups                                                                    */
/* ------------------------------------------------------------------------- */

size_t links_group_count(void)
{
    lock();
    const size_t n = s_group_count;
    unlock();
    return n;
}

bool links_group_get_at(size_t index, netdash_group_t *out)
{
    bool ok = false;
    lock();
    if (index < s_group_count && out != NULL) {
        *out = s_groups[index];
        ok   = true;
    }
    unlock();
    return ok;
}

esp_err_t links_group_add(const char *name, uint8_t *out_id)
{
    lock();
    if (s_group_count >= NETDASH_MAX_GROUPS) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    netdash_group_t *g = &s_groups[s_group_count];
    memset(g, 0, sizeof(*g));
    if (!set_group_name(g, name)) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Ids are never reused, so a link that still points at a deleted group
     * cannot be silently adopted by a new one that happened to land on the
     * same number. Wrapping past 255 is not a real case: there are eight
     * slots, and it would take 255 create/delete cycles to get there.
     */
    g->id = s_next_group_id;
    if (s_next_group_id < 255) {
        s_next_group_id++;
    }
    s_group_count++;

    if (out_id != NULL) {
        *out_id = g->id;
    }
    save_locked();
    unlock();
    return ESP_OK;
}

esp_err_t links_group_rename(uint8_t id, const char *name)
{
    lock();
    const int idx = find_group_locked(id);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    netdash_group_t copy = s_groups[idx];
    if (!set_group_name(&copy, name)) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    s_groups[idx] = copy;

    save_locked();
    unlock();
    return ESP_OK;
}

esp_err_t links_group_remove(uint8_t id)
{
    lock();
    const int idx = find_group_locked(id);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = (size_t)idx; i + 1 < s_group_count; i++) {
        s_groups[i] = s_groups[i + 1];
    }
    s_group_count--;
    memset(&s_groups[s_group_count], 0, sizeof(s_groups[s_group_count]));

    /* Deleting a heading must never delete the links filed under it. */
    reconcile_groups_locked();

    save_locked();
    unlock();
    return ESP_OK;
}

esp_err_t links_group_reorder(const uint8_t *ids, size_t count)
{
    if (ids == NULL || count > NETDASH_MAX_GROUPS) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (count != s_group_count) {
        unlock();
        return ESP_ERR_INVALID_ARG;   /* a permutation, not a bulk delete */
    }

    netdash_group_t next[NETDASH_MAX_GROUPS];
    for (size_t i = 0; i < count; i++) {
        const int idx = find_group_locked(ids[i]);
        if (idx < 0) {
            unlock();
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; j++) {
            if (next[j].id == ids[i]) {
                unlock();
                return ESP_ERR_INVALID_ARG;   /* duplicate id */
            }
        }
        next[i] = s_groups[idx];
    }

    memset(s_groups, 0, sizeof(s_groups));
    memcpy(s_groups, next, count * sizeof(next[0]));

    save_locked();
    unlock();
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */

netdash_scheme_t links_default_scheme(uint16_t port)
{
    switch (port) {
    case 443:
    case 8443:
    case 8006:
    case 9090:
    case 5001:
        return NETDASH_SCHEME_HTTPS;
    default:
        return NETDASH_SCHEME_HTTP;
    }
}

const char *netdash_scheme_name(netdash_scheme_t scheme)
{
    return scheme == NETDASH_SCHEME_HTTPS ? "https" : "http";
}
