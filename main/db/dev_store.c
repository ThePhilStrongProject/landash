/*
 * The device register. See dev_store.h.
 */
#include "dev_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "icons.h"

static const char *TAG = "dev_store";

#define STORE_PATH NETDASH_STORAGE_BASE "/devices.db"

_Static_assert(sizeof(dev_rec_t) == 192, "the device record layout is persisted");

static SemaphoreHandle_t s_lock;
static bool              s_ready;
static size_t            s_slots;                  /* slots in the file      */
static size_t            s_used;
/* Fingerprint of each slot's key; 0 means the slot is free. */
static uint32_t          s_hash[DEV_STORE_MAX];
/*
 * Scratch records, used only with the lock held. Callers include the HTTP
 * server, whose task stack has no room for several 192-byte records at once.
 */
static dev_rec_t         s_work;     /* the record being edited             */
static dev_rec_t         s_probe;    /* victim search and eviction          */

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static uint32_t key_hash(const uint8_t id[6])
{
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 6; i++) {
        h ^= id[i];
        h *= 0x01000193u;
    }
    return h == 0 ? 1 : h;   /* 0 is reserved for "free" */
}

/* Caller holds the lock. f is open. */
static bool read_slot_locked(FILE *f, size_t i, dev_rec_t *out)
{
    return fseek(f, (long)(i * sizeof(dev_rec_t)), SEEK_SET) == 0 &&
           fread(out, sizeof(*out), 1, f) == 1;
}

static bool write_slot_locked(FILE *f, size_t i, const dev_rec_t *rec)
{
    return fseek(f, (long)(i * sizeof(dev_rec_t)), SEEK_SET) == 0 &&
           fwrite(rec, sizeof(*rec), 1, f) == 1;
}

/* Caller holds the lock. Slot holding id, or -1. */
static int find_locked(FILE *f, const uint8_t id[6], dev_rec_t *scratch)
{
    const uint32_t h = key_hash(id);
    for (size_t i = 0; i < s_slots; i++) {
        if (s_hash[i] != h) {
            continue;
        }
        if (read_slot_locked(f, i, scratch) && scratch->magic == DEV_REC_MAGIC &&
            memcmp(scratch->mac, id, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t dev_store_init(bool *out_created)
{
    if (out_created != NULL) {
        *out_created = false;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!icons_available()) {
        ESP_LOGE(TAG, "storage partition unavailable; devices will not be remembered");
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    struct stat st;
    if (stat(STORE_PATH, &st) != 0) {
        FILE *c = fopen(STORE_PATH, "wb");
        if (c == NULL) {
            unlock();
            ESP_LOGE(TAG, "cannot create %s", STORE_PATH);
            return ESP_FAIL;
        }
        fclose(c);
        if (out_created != NULL) {
            *out_created = true;
        }
        st.st_size = 0;
    }

    s_slots = (size_t)st.st_size / sizeof(dev_rec_t);
    if (s_slots > DEV_STORE_MAX) {
        s_slots = DEV_STORE_MAX;
    }
    s_used = 0;
    memset(s_hash, 0, sizeof(s_hash));

    FILE *f = fopen(STORE_PATH, "rb");
    if (f != NULL) {
        for (size_t i = 0; i < s_slots; i++) {
            if (read_slot_locked(f, i, &s_work) && s_work.magic == DEV_REC_MAGIC) {
                s_hash[i] = key_hash(s_work.mac);
                s_used++;
            }
        }
        fclose(f);
    }
    s_ready = true;
    unlock();

    ESP_LOGI(TAG, "register: %u device(s) in %u slot(s), room for %u",
             (unsigned)s_used, (unsigned)s_slots, (unsigned)DEV_STORE_MAX);
    return ESP_OK;
}

bool dev_store_available(void)
{
    return s_ready;
}

size_t dev_store_count(void)
{
    if (!s_ready) {
        return 0;
    }
    lock();
    const size_t n = s_used;
    unlock();
    return n;
}

size_t dev_store_slots(void)
{
    if (!s_ready) {
        return 0;
    }
    lock();
    const size_t n = s_slots;
    unlock();
    return n;
}

bool dev_store_get(const uint8_t id[6], dev_rec_t *out)
{
    if (!s_ready || id == NULL || out == NULL) {
        return false;
    }
    lock();
    bool  found = false;
    FILE *f     = fopen(STORE_PATH, "rb");
    if (f != NULL) {
        found = find_locked(f, id, out) >= 0;
        fclose(f);
    }
    unlock();
    return found;
}

bool dev_store_read_slot(size_t i, dev_rec_t *out)
{
    if (!s_ready || out == NULL) {
        return false;
    }
    lock();
    bool ok = false;
    if (i < s_slots && s_hash[i] != 0) {
        FILE *f = fopen(STORE_PATH, "rb");
        if (f != NULL) {
            ok = read_slot_locked(f, i, out) && out->magic == DEV_REC_MAGIC;
            fclose(f);
        }
    }
    unlock();
    return ok;
}

/*
 * The slot to give up when the register is full: the record not seen for
 * longest among those without a nickname. Caller holds the lock.
 */
static int victim_locked(FILE *f)
{
    int        victim = -1;
    int64_t    oldest = 0;
    dev_rec_t *rec    = &s_probe;
    for (size_t i = 0; i < s_slots; i++) {
        if (s_hash[i] == 0 || !read_slot_locked(f, i, rec) || rec->magic != DEV_REC_MAGIC ||
            rec->nickname[0] != '\0') {
            continue;
        }
        const int64_t seen = rec->last_seen != 0 ? rec->last_seen : rec->first_seen;
        if (victim < 0 || seen < oldest) {
            victim = (int)i;
            oldest = seen;
        }
    }
    return victim;
}

/*
 * A slot for a new record: a free one, else a new one at the end of the file,
 * else the victim's. Caller holds the lock.
 */
static int alloc_slot_locked(FILE *f, uint8_t out_evicted[6], bool *out_did_evict)
{
    for (size_t i = 0; i < s_slots; i++) {
        if (s_hash[i] == 0) {
            return (int)i;
        }
    }
    if (s_slots < DEV_STORE_MAX) {
        return (int)s_slots;   /* grow the file by one record */
    }
    const int slot = victim_locked(f);
    if (slot >= 0 && read_slot_locked(f, (size_t)slot, &s_probe)) {
        if (out_evicted != NULL) {
            memcpy(out_evicted, s_probe.mac, 6);
        }
        if (out_did_evict != NULL) {
            *out_did_evict = true;
        }
        s_hash[slot] = 0;
        s_used--;
        ESP_LOGW(TAG, "register full; forgetting the device unseen longest");
    }
    return slot;
}

/* Writes rec to slot and updates the index. Caller holds the lock. */
static esp_err_t commit_locked(FILE *f, int slot, dev_rec_t *rec, bool fresh)
{
    rec->magic   = DEV_REC_MAGIC;
    rec->version = DEV_REC_VERSION;
    if (!write_slot_locked(f, (size_t)slot, rec) || fflush(f) != 0) {
        return ESP_FAIL;
    }
    if ((size_t)slot >= s_slots) {
        s_slots = (size_t)slot + 1;
    }
    if (fresh) {
        s_used++;
    }
    s_hash[slot] = key_hash(rec->mac);
    return ESP_OK;
}

static void log_failure(const uint8_t *id, esp_err_t err)
{
    ESP_LOGE(TAG, "could not store %02x:%02x:%02x:%02x:%02x:%02x: %s", id[0], id[1], id[2],
             id[3], id[4], id[5], esp_err_to_name(err));
}

esp_err_t dev_store_update(const uint8_t id[6], bool create, dev_store_edit_fn fn, void *ctx,
                           uint8_t out_evicted[6], bool *out_did_evict)
{
    if (out_did_evict != NULL) {
        *out_did_evict = false;
    }
    if (!s_ready || id == NULL || fn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    FILE *f = fopen(STORE_PATH, "r+b");
    if (f == NULL) {
        unlock();
        return ESP_FAIL;
    }

    dev_rec_t *rec    = &s_work;
    int       slot    = find_locked(f, id, rec);
    bool      created = false;
    esp_err_t err     = ESP_OK;
    if (slot < 0) {
        if (!create) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            memset(rec, 0, sizeof(*rec));
            memcpy(rec->mac, id, 6);
            created = true;
            slot    = alloc_slot_locked(f, out_evicted, out_did_evict);
            if (slot < 0) {
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (err == ESP_OK && fn(rec, created, ctx)) {
        memcpy(rec->mac, id, 6);   /* the key is not fn's to change */
        err = commit_locked(f, slot, rec, created);
    }
    fclose(f);
    unlock();

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        log_failure(id, err);
    }
    return err;
}

static bool replace_with(dev_rec_t *rec, bool created, void *ctx)
{
    (void)created;
    *rec = *(const dev_rec_t *)ctx;
    return true;
}

esp_err_t dev_store_put(const dev_rec_t *rec, uint8_t out_evicted[6], bool *out_did_evict)
{
    if (rec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return dev_store_update(rec->mac, true, replace_with, (void *)rec, out_evicted,
                            out_did_evict);
}

esp_err_t dev_store_remove(const uint8_t id[6])
{
    if (!s_ready || id == NULL) {
        return ESP_OK;
    }
    lock();
    esp_err_t err = ESP_OK;
    FILE     *f   = fopen(STORE_PATH, "r+b");
    if (f != NULL) {
        const int slot = find_locked(f, id, &s_work);
        if (slot >= 0) {
            memset(&s_work, 0, sizeof(s_work));   /* magic 0: free */
            if (write_slot_locked(f, (size_t)slot, &s_work) && fflush(f) == 0) {
                s_hash[slot] = 0;
                s_used--;
            } else {
                err = ESP_FAIL;
            }
        }
        fclose(f);
    } else {
        err = ESP_FAIL;
    }
    unlock();
    return err;
}
