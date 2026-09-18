/*
 * Dashboard quick links. See links.h for why a link stores a MAC, not an IP.
 *
 * The whole list is one NVS blob rather than a key per link: it is at most
 * 24 x 44 bytes, it is rewritten on any change anyway because order matters,
 * and a single blob makes reordering atomic.
 */
#include "links.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "links";

#define LINKS_NVS_NS   "links"
#define LINKS_KEY      "list"
#define LINKS_VERSION  1

typedef struct __attribute__((packed)) {
    uint8_t        version;
    uint8_t        count;
    uint16_t       next_id;
    netdash_link_t items[NETDASH_MAX_LINKS];
} links_blob_t;

static netdash_link_t   s_links[NETDASH_MAX_LINKS];
static size_t           s_count;
static uint16_t         s_next_id = 1;
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
        .version = LINKS_VERSION,
        .count   = (uint8_t)s_count,
        .next_id = s_next_id,
    };
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

esp_err_t links_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock();
    s_count   = 0;
    s_next_id = 1;

    nvs_handle_t h;
    if (nvs_open(LINKS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        links_blob_t blob;
        size_t       size = sizeof(blob);

        if (nvs_get_blob(h, LINKS_KEY, &blob, &size) == ESP_OK && size == sizeof(blob) &&
            blob.version == LINKS_VERSION) {
            s_count = blob.count > NETDASH_MAX_LINKS ? NETDASH_MAX_LINKS : blob.count;
            memcpy(s_links, blob.items, sizeof(s_links));
            s_next_id = blob.next_id != 0 ? blob.next_id : 1;

            /* Defend against a truncated label in a hand-edited blob. */
            for (size_t i = 0; i < s_count; i++) {
                s_links[i].label[NETDASH_LINK_LABEL - 1] = '\0';
            }
        }
        nvs_close(h);
    }
    const size_t loaded = s_count;
    unlock();

    ESP_LOGI(TAG, "loaded %u dashboard link(s)", (unsigned)loaded);
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

esp_err_t links_update(uint16_t id, const char *label, uint16_t port, int scheme)
{
    lock();
    const int idx = find_locked(id);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (label != NULL) {
        memset(s_links[idx].label, 0, sizeof(s_links[idx].label));
        strncpy(s_links[idx].label, label, sizeof(s_links[idx].label) - 1);
    }
    if (port != UINT16_MAX && port != 0) {
        s_links[idx].port = port;
    }
    if (scheme >= 0) {
        s_links[idx].scheme = (uint8_t)scheme;
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
    return ESP_OK;
}

esp_err_t links_reorder(const uint16_t *ids, size_t count)
{
    if (ids == NULL || count > NETDASH_MAX_LINKS) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();

    /* Build the new list first, so an unknown id leaves the old one intact. */
    netdash_link_t next[NETDASH_MAX_LINKS];
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

    memset(s_links, 0, sizeof(s_links));
    memcpy(s_links, next, count * sizeof(next[0]));
    s_count = count;

    save_locked();
    unlock();
    return ESP_OK;
}

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
