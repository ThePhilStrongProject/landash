/*
 * Uploaded dashboard icons. See icons.h for the split of work with the
 * browser and for why ids are never reused.
 */
#include "icons.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "icons";

#define ICON_BASE      NETDASH_STORAGE_BASE
#define ICON_PART      "storage"
#define ICON_NVS_NS    "icons"
#define ICON_NEXT_KEY  "next"

/* Streamed in and out in chunks this size; two of these is the whole cost. */
#define ICON_CHUNK     1024

typedef struct {
    uint16_t id;
    uint32_t bytes;
} icon_rec_t;

static icon_rec_t        s_icons[NETDASH_MAX_ICONS];
static size_t            s_count;
static uint16_t          s_next_id = 1;
static bool              s_mounted;
static SemaphoreHandle_t s_lock;

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

void icons_file_name(uint16_t id, char *out, size_t cap)
{
    snprintf(out, cap, "%u.png", (unsigned)id);
}

static void path_for(uint16_t id, char *out, size_t cap)
{
    char name[16];
    icons_file_name(id, name, sizeof(name));
    snprintf(out, cap, ICON_BASE "/%s", name);
}

/* Caller holds the lock. -1 when the id is not present. */
static int find_locked(uint16_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_icons[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static void save_next_id(uint16_t next)
{
    nvs_handle_t h;
    if (nvs_open(ICON_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u16(h, ICON_NEXT_KEY, next) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Caller holds the lock. Rebuilds the index from what is actually on disk. */
static void index_locked(void)
{
    s_count = 0;

    DIR *d = opendir(ICON_BASE);
    if (d == NULL) {
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < NETDASH_MAX_ICONS) {
        /* Names are "<id>.png" and nothing else ever writes here. */
        char *end = NULL;
        long  id  = strtol(e->d_name, &end, 10);
        if (end == e->d_name || id < 1 || id > 65534 || strcmp(end, ".png") != 0) {
            continue;
        }

        char path[48];
        path_for((uint16_t)id, path, sizeof(path));

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        s_icons[s_count].id    = (uint16_t)id;
        s_icons[s_count].bytes = (uint32_t)st.st_size;
        s_count++;

        if ((uint16_t)id >= s_next_id) {
            s_next_id = (uint16_t)(id + 1);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------------------------- */

static esp_err_t mount(void)
{
    const esp_vfs_spiffs_conf_t conf = {
        .base_path              = ICON_BASE,
        .partition_label        = ICON_PART,
        .max_files              = 6,   /* icons, plus the device register */
        .format_if_mount_failed = true,
    };
    return esp_vfs_spiffs_register(&conf);
}

esp_err_t icons_storage_claim_empty(void)
{
    esp_err_t err = mount();
    if (err == ESP_OK) {
        err = esp_spiffs_format(ICON_PART);
        if (err != ESP_OK) {
            esp_vfs_spiffs_unregister(ICON_PART);
        }
    }
    return err;
}

void icons_storage_release(void)
{
    esp_vfs_spiffs_unregister(ICON_PART);
}

esp_err_t icons_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = mount();
    if (err != ESP_OK) {
        /* A dashboard without uploaded icons is still a dashboard. */
        ESP_LOGW(TAG, "storage unavailable (%s); uploaded icons are disabled",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    lock();
    s_mounted = true;

    nvs_handle_t h;
    if (nvs_open(ICON_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t next = 0;
        if (nvs_get_u16(h, ICON_NEXT_KEY, &next) == ESP_OK && next > 0) {
            s_next_id = next;
        }
        nvs_close(h);
    }

    /* Scanning can only push s_next_id up, never down, so an id can never be
       handed out twice even if the stored counter was lost. */
    index_locked();
    const size_t n = s_count;
    const uint16_t next = s_next_id;
    unlock();

    save_next_id(next);

    size_t total = 0, used = 0;
    esp_spiffs_info(ICON_PART, &total, &used);
    ESP_LOGI(TAG, "storage mounted: %u icon(s), %u of %u bytes used",
             (unsigned)n, (unsigned)used, (unsigned)total);
    return ESP_OK;
}

bool icons_available(void)
{
    lock();
    const bool ok = s_mounted;
    unlock();
    return ok;
}

/*
 * A PNG starts with a fixed 8-byte signature and its first chunk is always
 * IHDR, whose width and height are big-endian 32-bit values at offsets 16 and
 * 20. That is enough to reject both "not a PNG at all" and "a PNG far larger
 * than the 64x64 this is for", the second of which compresses small enough to
 * slip under the byte cap if it is mostly flat colour.
 */
static bool png_header_ok(const uint8_t *buf, size_t len)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    if (len < 24 || memcmp(buf, sig, sizeof(sig)) != 0) {
        return false;
    }
    if (memcmp(buf + 12, "IHDR", 4) != 0) {
        return false;
    }
    const uint32_t w = ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) |
                       ((uint32_t)buf[18] << 8) | buf[19];
    const uint32_t h = ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) |
                       ((uint32_t)buf[22] << 8) | buf[23];

    return w >= 1 && h >= 1 && w <= NETDASH_ICON_PX && h <= NETDASH_ICON_PX;
}

esp_err_t icons_store(int (*read_fn)(void *ctx, char *buf, size_t len), void *ctx,
                      uint16_t *out_id)
{
    if (read_fn == NULL || out_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (!s_mounted) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count >= NETDASH_MAX_ICONS) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    const uint16_t id = s_next_id;
    unlock();

    char path[48];
    path_for(id, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return ESP_FAIL;
    }

    uint8_t  *buf     = malloc(ICON_CHUNK);
    uint8_t   head[24];
    size_t    head_n  = 0;
    size_t    total   = 0;
    esp_err_t err     = ESP_OK;

    if (buf == NULL) {
        fclose(f);
        unlink(path);
        return ESP_ERR_NO_MEM;
    }

    for (;;) {
        const int n = read_fn(ctx, (char *)buf, ICON_CHUNK);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }

        /* Keep the first 24 bytes aside so the header can be checked once the
           whole upload has arrived - a short first chunk is legal. */
        if (head_n < sizeof(head)) {
            const size_t take = sizeof(head) - head_n < (size_t)n ? sizeof(head) - head_n
                                                                  : (size_t)n;
            memcpy(head + head_n, buf, take);
            head_n += take;
        }

        total += (size_t)n;
        if (total > NETDASH_ICON_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            ESP_LOGE(TAG, "write failed, storage is probably full");
            err = ESP_FAIL;
            break;
        }
    }

    free(buf);
    fclose(f);

    if (err == ESP_OK && total == 0) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && !png_header_ok(head, head_n)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK) {
        unlink(path);
        return err;
    }

    lock();
    if (s_count >= NETDASH_MAX_ICONS) {
        /* Another upload filled the last slot while this one was streaming. */
        unlock();
        unlink(path);
        return ESP_ERR_NO_MEM;
    }
    s_icons[s_count].id    = id;
    s_icons[s_count].bytes = (uint32_t)total;
    s_count++;
    if (s_next_id < 65534) {
        s_next_id++;
    }
    const uint16_t next = s_next_id;
    unlock();

    save_next_id(next);

    *out_id = id;
    ESP_LOGI(TAG, "stored icon %u (%u bytes)", (unsigned)id, (unsigned)total);
    return ESP_OK;
}

size_t icons_count(void)
{
    lock();
    const size_t n = s_count;
    unlock();
    return n;
}

bool icons_get_at(size_t index, uint16_t *out_id, size_t *out_bytes)
{
    bool ok = false;
    lock();
    if (index < s_count) {
        if (out_id != NULL) {
            *out_id = s_icons[index].id;
        }
        if (out_bytes != NULL) {
            *out_bytes = s_icons[index].bytes;
        }
        ok = true;
    }
    unlock();
    return ok;
}

bool icons_exists(uint16_t id)
{
    lock();
    const bool ok = s_mounted && find_locked(id) >= 0;
    unlock();
    return ok;
}

esp_err_t icons_delete(uint16_t id)
{
    lock();
    if (!s_mounted) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const int idx = find_locked(id);
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = (size_t)idx; i + 1 < s_count; i++) {
        s_icons[i] = s_icons[i + 1];
    }
    s_count--;
    unlock();

    char path[48];
    path_for(id, path, sizeof(path));
    unlink(path);

    ESP_LOGI(TAG, "deleted icon %u", (unsigned)id);
    return ESP_OK;
}

FILE *icons_open(uint16_t id, size_t *out_bytes)
{
    if (!icons_exists(id)) {
        return NULL;
    }

    char path[48];
    path_for(id, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f != NULL && out_bytes != NULL) {
        *out_bytes = (size_t)st.st_size;
    }
    return f;
}

esp_err_t icons_factory_reset(void)
{
    lock();
    if (!s_mounted) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_count = 0;
    memset(s_icons, 0, sizeof(s_icons));
    unlock();

    /* Format rather than unlink each file: this runs moments before a restart
       and a fresh filesystem is the point. */
    const esp_err_t err = esp_spiffs_format(ICON_PART);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not format storage: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "storage formatted, all uploaded icons destroyed");
    }
    return err;
}

void icons_usage(size_t *out_used, size_t *out_total)
{
    size_t total = 0, used = 0;

    if (icons_available()) {
        esp_spiffs_info(ICON_PART, &total, &used);
    }
    if (out_used != NULL) {
        *out_used = used;
    }
    if (out_total != NULL) {
        *out_total = total;
    }
}
