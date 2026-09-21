/*
 * NetDash notification feed. See notify.h for what separates this from the
 * events log.
 *
 * The ring is stored oldest-first and shifted down when it overflows. A ring
 * with a head index would avoid the memmove, but the whole array is 3.8 KB and
 * the shift only happens once the feed is full, which on a quiet network is a
 * few times a week - not worth the off-by-one risk of a wrapped layout that
 * also has to survive being written to flash.
 */
#include "notify.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "notify";

#define NOTIF_NVS_NS   "notif"
#define NOTIF_KEY      "ring"
#define NOTIF_VERSION  1

/* A sweep can land several notifications at once; coalesce the flash writes. */
#define NOTIF_FLUSH_MS 10000

typedef struct __attribute__((packed)) {
    uint8_t         version;
    uint8_t         count;
    uint32_t        next_id;
    netdash_notif_t items[NETDASH_NOTIF_RING];
} notif_blob_t;

static netdash_notif_t   s_items[NETDASH_NOTIF_RING];  /* [0] is the oldest */
static size_t            s_count;
static uint32_t          s_next_id = 1;
static bool              s_dirty;
static bool              s_armed;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_flush_timer;

/* ------------------------------------------------------------------------- */

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

/*
 * One scratch copy of the on-disk layout, shared by the loader and the
 * flusher. At 3.8 KB it is too big for a task stack and not worth having two
 * of; the two users cannot overlap, because load_locked() only runs from
 * notify_init() and flush_if_dirty() only from the flush timer.
 */
static notif_blob_t s_scratch;

/* Builds the blob under the lock, then writes it without holding it. */
static void flush_if_dirty(void)
{
    lock();
    if (!s_dirty) {
        unlock();
        return;
    }
    s_dirty             = false;
    s_scratch.version   = NOTIF_VERSION;
    s_scratch.count     = (uint8_t)s_count;
    s_scratch.next_id   = s_next_id;
    memcpy(s_scratch.items, s_items, sizeof(s_scratch.items));
    unlock();

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NOTIF_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, NOTIF_KEY, &s_scratch, sizeof(s_scratch));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "feed not saved: %s", esp_err_to_name(err));
    }
}

static void flush_cb(void *arg)
{
    (void)arg;
    flush_if_dirty();
}

static void load_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NOTIF_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    size_t size = sizeof(s_scratch);

    if (nvs_get_blob(h, NOTIF_KEY, &s_scratch, &size) == ESP_OK &&
        size == sizeof(s_scratch) && s_scratch.version == NOTIF_VERSION) {
        s_count = s_scratch.count > NETDASH_NOTIF_RING ? NETDASH_NOTIF_RING : s_scratch.count;
        memcpy(s_items, s_scratch.items, sizeof(s_items));
        s_next_id = s_scratch.next_id != 0 ? s_scratch.next_id : 1;

        /* Defend against a truncated string in a hand-edited blob. */
        for (size_t i = 0; i < s_count; i++) {
            s_items[i].text[NETDASH_NOTIF_TEXT - 1] = '\0';
        }
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------------- */

esp_err_t notify_init(void)
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
    s_dirty   = false;
    memset(s_items, 0, sizeof(s_items));
    load_locked();
    const size_t loaded = s_count;
    unlock();

    if (s_flush_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = flush_cb,
            .name     = "notif_flush",
        };
        esp_err_t err = esp_timer_create(&args, &s_flush_timer);
        if (err == ESP_OK) {
            err = esp_timer_start_periodic(s_flush_timer, (uint64_t)NOTIF_FLUSH_MS * 1000);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush timer unavailable (%s), feed will not persist",
                     esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "loaded %u notification(s)", (unsigned)loaded);
    return ESP_OK;
}

void notify_arm(void)
{
    lock();
    const bool was = s_armed;
    s_armed        = true;
    unlock();

    if (!was) {
        ESP_LOGI(TAG, "feed armed");
    }
}

bool notify_armed(void)
{
    lock();
    const bool armed = s_armed;
    unlock();
    return armed;
}

void notify_push(netdash_notif_type_t type, const uint8_t mac[6], uint32_t ip, const char *text)
{
    if (type >= NETDASH_NOTIF_COUNT) {
        return;
    }

    netdash_settings_t cfg;
    settings_get(&cfg);
    if ((cfg.notif_mask & NETDASH_NOTIF_BIT(type)) == 0) {
        return;
    }

    lock();
    if (!s_armed) {
        unlock();
        return;
    }

    if (s_count == NETDASH_NOTIF_RING) {
        memmove(&s_items[0], &s_items[1], sizeof(s_items[0]) * (NETDASH_NOTIF_RING - 1));
        s_count--;
    }

    netdash_notif_t *n = &s_items[s_count++];
    memset(n, 0, sizeof(*n));
    n->id   = s_next_id++;
    n->ts   = (int64_t)time(NULL);
    n->type = (uint8_t)type;
    n->read = 0;
    n->ip   = ip;
    if (mac != NULL) {
        memcpy(n->mac, mac, 6);
    }
    if (text != NULL) {
        strncpy(n->text, text, sizeof(n->text) - 1);
    }
    s_dirty = true;
    unlock();
}

bool notify_get(size_t index, netdash_notif_t *out)
{
    bool ok = false;
    lock();
    if (out != NULL && index < s_count) {
        *out = s_items[s_count - 1 - index];   /* index 0 is the newest */
        ok   = true;
    }
    unlock();
    return ok;
}

size_t notify_count(void)
{
    lock();
    const size_t n = s_count;
    unlock();
    return n;
}

size_t notify_unread(void)
{
    size_t n = 0;
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (!s_items[i].read) {
            n++;
        }
    }
    unlock();
    return n;
}

esp_err_t notify_mark_read(uint32_t id)
{
    bool found = (id == 0);

    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (id == 0 || s_items[i].id == id) {
            if (!s_items[i].read) {
                s_items[i].read = 1;
                s_dirty         = true;
            }
            if (id != 0) {
                found = true;
                break;
            }
        }
    }
    unlock();

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t notify_dismiss(uint32_t id)
{
    bool found = false;

    lock();
    if (id == 0) {
        found   = true;
        s_count = 0;
        memset(s_items, 0, sizeof(s_items));
        s_dirty = true;
    } else {
        for (size_t i = 0; i < s_count; i++) {
            if (s_items[i].id == id) {
                memmove(&s_items[i], &s_items[i + 1], sizeof(s_items[0]) * (s_count - i - 1));
                s_count--;
                memset(&s_items[s_count], 0, sizeof(s_items[s_count]));
                s_dirty = true;
                found   = true;
                break;
            }
        }
    }
    unlock();

    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const char *netdash_notif_type_name(netdash_notif_type_t type)
{
    switch (type) {
    case NETDASH_NOTIF_NEW_DEVICE:  return "new_device";
    case NETDASH_NOTIF_IP_CHANGED:  return "ip_changed";
    case NETDASH_NOTIF_NEW_PORT:    return "new_port";
    case NETDASH_NOTIF_DEVICE_GONE: return "device_gone";
    case NETDASH_NOTIF_DEVICE_BACK: return "device_back";
    case NETDASH_NOTIF_WAN_DOWN:    return "wan_down";
    case NETDASH_NOTIF_WAN_UP:      return "wan_up";
    case NETDASH_NOTIF_UPDATE:      return "update";
    default:                        return "unknown";
    }
}

bool netdash_notif_type_from_name(const char *name, netdash_notif_type_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int t = 0; t < NETDASH_NOTIF_COUNT; t++) {
        if (strcmp(name, netdash_notif_type_name((netdash_notif_type_t)t)) == 0) {
            *out = (netdash_notif_type_t)t;
            return true;
        }
    }
    return false;
}
