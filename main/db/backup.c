/*
 * Backup and restore. See backup.h for what is carried and why, and
 * docs/API.md for the file format, which the web installer reads too.
 */
#include "backup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "device_db.h"
#include "icons.h"
#include "links.h"
#include "ota.h"
#include "settings.h"

static const char *TAG = "backup";

#define FILE_MAGIC      "LANDASH\n"
#define FILE_MAGIC_LEN  8
#define FILE_FORMAT     1
#define HDR_MAX         1024
#define PREFIX_MAX      (FILE_MAGIC_LEN + 2 + HDR_MAX)

#define REC_MAX         8192        /* plaintext bytes in one record          */
#define FILE_CHUNK      4096        /* file bytes in one 'D' record           */
#define FILE_MAX_BYTES  (1024u * 1024u)   /* nothing on a 1 MB volume is bigger */

#define REC_NVS         'N'
#define REC_FILE        'F'
#define REC_DATA        'D'
#define REC_END         'E'

/*
 * The same derivation as the vault, and the same reasoning: the rounds are
 * the whole cost of guessing a passphrase from a stolen file. The count is in
 * the header, so it can rise later; the ceiling stops a crafted header from
 * tying the web server up for minutes.
 */
#define KDF_ITERS       40000
#define KDF_ITERS_MIN   10000
#define KDF_ITERS_MAX   400000
#define SALT_LEN        16
#define NONCE_LEN       8
#define KEY_LEN         32
#define IV_LEN          12
#define GCM_TAG_LEN     16
#define AAD_LEN         32

/* Staging in the idle update slot: one header sector, then the records. */
#define SECTOR          4096
#define STAGE_MAGIC     "LDSTAGE1"
#define STAGE_DATA_OFF  SECTOR

#define STATUS_NS       "backup"
#define STATUS_LAST     "last"
#define STATUS_FROM     "from"
#define STATUS_FROM_VER 1

/*
 * Where a user's work lives in NVS. A module that adds a namespace worth
 * keeping adds it here, as it does to the factory reset's list in
 * http_server.c. Left out on purpose: "notif" (the feed) and "ota" (update
 * bookkeeping) belong to the old dongle's life, not its setup.
 */
static const char *const k_namespaces[] = {
    "cfg",    /* settings, Wi-Fi and setup-AP passwords included */
    "links",  /* dashboard links and groups                      */
    "note",   /* notes on devices                                */
    "lnote",  /* notes on links                                  */
    "sec",    /* secrets on devices, still vault-encrypted       */
    "lsec",   /* credentials on links, likewise                  */
    "vault",  /* vault salt and verifier                         */
    "icons",  /* uploaded icon id counter                        */
};

/*
 * Not carried, but wiped by a restore: the storage from before v0.16, which
 * device_db copies into a register it has to create. A backup made without a
 * register would otherwise have this dongle's old devices appear in it.
 */
static const char *const k_legacy_namespaces[] = {"dev", "ports"};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

typedef struct __attribute__((packed)) {
    char     magic[8];
    uint32_t data_len;
    uint32_t records;
    uint8_t  sha[32];
    char     fw[32];
    char     hostname[32];
    int64_t  created;
    uint32_t devices;
    uint32_t links;
    uint32_t icons;
} stage_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    char     fw[32];
    char     hostname[32];
    int64_t  created;
    uint32_t devices;
    uint32_t links;
    uint32_t icons;
} status_from_t;

_Static_assert(sizeof(stage_hdr_t) <= SECTOR, "the staging header is one sector");

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Exactly n bytes of hex, nothing more. */
static bool from_hex(const char *s, uint8_t *out, size_t n)
{
    if (s == NULL || strlen(s) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const int hi = hex_nib(s[i * 2]);
        const int lo = hex_nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", msg);
    }
}

static bool namespace_carried(const char *ns)
{
    for (size_t i = 0; i < COUNT_OF(k_namespaces); i++) {
        if (strcmp(ns, k_namespaces[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool nvs_int_type(uint8_t type)
{
    switch (type) {
    case NVS_TYPE_U8:  case NVS_TYPE_I8:
    case NVS_TYPE_U16: case NVS_TYPE_I16:
    case NVS_TYPE_U32: case NVS_TYPE_I32:
    case NVS_TYPE_U64: case NVS_TYPE_I64:
        return true;
    default:
        return false;
    }
}

/* The low nibble of an integer nvs_type_t is its width in bytes. */
static size_t nvs_int_width(uint8_t type)
{
    return type & 0x0f;
}

/*
 * A file name the restore may create on the storage partition: no path, no
 * dot files, only characters the register and the icons actually use.
 */
static bool file_name_ok(const char *name, size_t len)
{
    if (len < 1 || len > 31 || name[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/*
 * The key derivation holds the CPU for a couple of seconds, and on a fast LAN
 * a whole upload is already buffered, so the HTTP task never blocks on its
 * own: two restores back to back starved the idle task past the watchdog's
 * five seconds, and a tick's sleep between them did not help, because the
 * tasks kept waiting by then used the whole tick. So the derivation runs at
 * idle priority, sharing the CPU with the idle task tick by tick and giving
 * way to everything else, and the record loops still pause now and then.
 */
static void breathe(void)
{
    vTaskDelay(1);
}

#define BREATHE_EVERY 8   /* records */

static esp_err_t derive_key(const char *pass, const uint8_t salt[SALT_LEN], uint32_t iters,
                            uint8_t out[KEY_LEN])
{
    const UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pass,
                                                 strlen(pass), salt, SALT_LEN, iters, KEY_LEN,
                                                 out);
    vTaskPrioritySet(NULL, prio);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

/* The file's nonce, then the record's index big-endian. */
static void make_iv(const uint8_t nonce[NONCE_LEN], uint32_t index, uint8_t iv[IV_LEN])
{
    memcpy(iv, nonce, NONCE_LEN);
    iv[8]  = (uint8_t)(index >> 24);
    iv[9]  = (uint8_t)(index >> 16);
    iv[10] = (uint8_t)(index >> 8);
    iv[11] = (uint8_t)index;
}

bool backup_passphrase_ok(const char *pass)
{
    if (pass == NULL) {
        return false;
    }
    const size_t len = strlen(pass);
    if (len < BACKUP_PASS_MIN || len > BACKUP_PASS_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)pass[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

void backup_filename(int64_t now, char *out, size_t cap)
{
    netdash_settings_t cfg;
    settings_get(&cfg);

    /* "landash-landash-..." for a dongle still on the default name says the
       same thing twice. */
    char stem[48];
    if (strcmp(cfg.hostname, "landash") == 0) {
        snprintf(stem, sizeof(stem), "landash");
    } else {
        snprintf(stem, sizeof(stem), "landash-%s", cfg.hostname);
    }

    if (now > 0) {
        const time_t t = (time_t)now;
        struct tm    tm;
        localtime_r(&t, &tm);
        snprintf(out, cap, "%s-%04d%02d%02d.landash", stem, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday);
    } else {
        snprintf(out, cap, "%s.landash", stem);
    }
}

/* ------------------------------------------------------------------------- */
/* Status                                                                    */
/* ------------------------------------------------------------------------- */

void backup_get_status(backup_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    if (nvs_open(STATUS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    (void)nvs_get_i64(h, STATUS_LAST, &out->last_backup);

    status_from_t from;
    size_t        size = sizeof(from);
    if (nvs_get_blob(h, STATUS_FROM, &from, &size) == ESP_OK && size == sizeof(from) &&
        from.version == STATUS_FROM_VER) {
        out->restored = true;
        snprintf(out->from.fw, sizeof(out->from.fw), "%.*s", (int)sizeof(from.fw), from.fw);
        snprintf(out->from.hostname, sizeof(out->from.hostname), "%.*s",
                 (int)sizeof(from.hostname), from.hostname);
        out->from.created = from.created;
        out->from.devices = from.devices;
        out->from.links   = from.links;
        out->from.icons   = from.icons;
    }
    nvs_close(h);
}

static void record_backup_time(int64_t now)
{
    nvs_handle_t h;
    if (now <= 0 || nvs_open(STATUS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i64(h, STATUS_LAST, now) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------------- */
/* Export                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    mbedtls_gcm_context gcm;
    uint8_t             nonce[NONCE_LEN];
    uint8_t             aad[AAD_LEN];
    uint32_t            index;          /* records written so far           */
    backup_write_fn     write;
    void               *ctx;
    uint8_t             pt[REC_MAX];
    uint8_t             frame[4 + REC_MAX + GCM_TAG_LEN];
} writer_t;

/* Encrypts the first len bytes of w->pt as the next record and writes it. */
static esp_err_t emit(writer_t *w, size_t len)
{
    uint8_t iv[IV_LEN];
    make_iv(w->nonce, w->index, iv);
    put_u32le(w->frame, (uint32_t)len);

    const int rc = mbedtls_gcm_crypt_and_tag(&w->gcm, MBEDTLS_GCM_ENCRYPT, len, iv, IV_LEN,
                                             w->aad, AAD_LEN, w->pt, w->frame + 4, GCM_TAG_LEN,
                                             w->frame + 4 + len);
    if (rc != 0) {
        ESP_LOGE(TAG, "encrypt failed (%d)", rc);
        return ESP_FAIL;
    }
    w->index++;
    if (w->index % BREATHE_EVERY == 0) {
        breathe();
    }
    return w->write(w->ctx, w->frame, 4 + len + GCM_TAG_LEN);
}

static esp_err_t read_nvs_int(nvs_handle_t h, const char *key, uint8_t type, uint64_t *out)
{
    esp_err_t err;
    switch (type) {
    case NVS_TYPE_U8:  { uint8_t v;  err = nvs_get_u8(h, key, &v);  *out = v;           break; }
    case NVS_TYPE_I8:  { int8_t v;   err = nvs_get_i8(h, key, &v);  *out = (uint8_t)v;  break; }
    case NVS_TYPE_U16: { uint16_t v; err = nvs_get_u16(h, key, &v); *out = v;           break; }
    case NVS_TYPE_I16: { int16_t v;  err = nvs_get_i16(h, key, &v); *out = (uint16_t)v; break; }
    case NVS_TYPE_U32: { uint32_t v; err = nvs_get_u32(h, key, &v); *out = v;           break; }
    case NVS_TYPE_I32: { int32_t v;  err = nvs_get_i32(h, key, &v); *out = (uint32_t)v; break; }
    case NVS_TYPE_U64: { uint64_t v; err = nvs_get_u64(h, key, &v); *out = v;           break; }
    case NVS_TYPE_I64: { int64_t v;  err = nvs_get_i64(h, key, &v); *out = (uint64_t)v; break; }
    default:           err = ESP_ERR_NOT_SUPPORTED; break;
    }
    return err;
}

static esp_err_t export_entry(writer_t *w, nvs_handle_t h, const char *ns,
                              const nvs_entry_info_t *info)
{
    uint8_t     *p  = w->pt;
    const size_t nl = strlen(ns);
    const size_t kl = strlen(info->key);
    size_t       n  = 0;

    p[n++] = REC_NVS;
    p[n++] = (uint8_t)nl;
    memcpy(p + n, ns, nl);
    n += nl;
    p[n++] = (uint8_t)kl;
    memcpy(p + n, info->key, kl);
    n += kl;
    p[n++] = (uint8_t)info->type;

    const size_t room = REC_MAX - n;
    esp_err_t    err;
    if (nvs_int_type((uint8_t)info->type)) {
        uint64_t v = 0;
        err = read_nvs_int(h, info->key, (uint8_t)info->type, &v);
        for (size_t i = 0; i < nvs_int_width((uint8_t)info->type); i++) {
            p[n++] = (uint8_t)(v >> (8 * i));
        }
    } else if (info->type == NVS_TYPE_STR) {
        size_t len = room;
        err = nvs_get_str(h, info->key, (char *)p + n, &len);
        n += len > 0 ? len - 1 : 0;   /* the NUL is not carried */
    } else if (info->type == NVS_TYPE_BLOB) {
        size_t len = room;
        err = nvs_get_blob(h, info->key, p + n, &len);
        n += len;
    } else {
        return ESP_OK;   /* nothing else is ever written */
    }

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "%s/%s is larger than a backup record holds", ns, info->key);
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot read %s/%s: %s", ns, info->key, esp_err_to_name(err));
        return err;
    }
    return emit(w, n);
}

static esp_err_t export_namespace(writer_t *w, const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;   /* never written: nothing to carry */
    }

    nvs_iterator_t it  = NULL;
    esp_err_t      err = ESP_OK;
    esp_err_t      at  = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    while (at == ESP_OK && err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        err = export_entry(w, h, ns, &info);
        at  = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    return err;
}

static esp_err_t emit_file_begin(writer_t *w, const char *name, uint32_t size)
{
    const size_t nl = strlen(name);
    w->pt[0] = REC_FILE;
    w->pt[1] = (uint8_t)nl;
    memcpy(w->pt + 2, name, nl);
    put_u32le(w->pt + 2 + nl, size);
    return emit(w, 2 + nl + 4);
}

/* The register, whole records at a time so none is caught half-written. */
static esp_err_t export_register(writer_t *w)
{
    const size_t slots = device_db_register_slots();
    if (slots == 0) {
        return ESP_OK;
    }
    esp_err_t err = emit_file_begin(w, DEVICE_DB_REGISTER_FILE,
                                    (uint32_t)(slots * DEVICE_DB_REGISTER_REC_BYTES));

    const size_t per = FILE_CHUNK / DEVICE_DB_REGISTER_REC_BYTES;
    for (size_t first = 0; err == ESP_OK && first < slots;) {
        const size_t want = slots - first < per ? slots - first : per;
        const size_t got  = device_db_register_read_raw(first, w->pt + 1, want);
        if (got != want) {
            ESP_LOGE(TAG, "register read stopped at slot %u of %u", (unsigned)(first + got),
                     (unsigned)slots);
            return ESP_FAIL;
        }
        w->pt[0] = REC_DATA;
        err      = emit(w, 1 + got * DEVICE_DB_REGISTER_REC_BYTES);
        first += got;
    }
    return err;
}

static esp_err_t export_icons(writer_t *w)
{
    const size_t n   = icons_count();
    esp_err_t    err = ESP_OK;

    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        uint16_t id;
        if (!icons_get_at(i, &id, NULL)) {
            break;   /* one was deleted meanwhile */
        }
        size_t bytes = 0;
        FILE  *f     = icons_open(id, &bytes);
        if (f == NULL) {
            continue;
        }

        char name[32];
        icons_file_name(id, name, sizeof(name));
        err = emit_file_begin(w, name, (uint32_t)bytes);

        size_t done = 0;
        while (err == ESP_OK && done < bytes) {
            const size_t want = bytes - done < FILE_CHUNK ? bytes - done : FILE_CHUNK;
            if (fread(w->pt + 1, 1, want, f) != want) {
                ESP_LOGE(TAG, "icon %u read short", (unsigned)id);
                err = ESP_FAIL;
                break;
            }
            w->pt[0] = REC_DATA;
            err      = emit(w, 1 + want);
            done += want;
        }
        fclose(f);
    }
    return err;
}

/* Writes the readable part of the file and keeps its hash as every record's AAD. */
static esp_err_t write_prefix(writer_t *w, const uint8_t salt[SALT_LEN], int64_t now)
{
    netdash_settings_t cfg;
    settings_get(&cfg);

    char salt_hex[SALT_LEN * 2 + 1];
    char nonce_hex[NONCE_LEN * 2 + 1];
    to_hex(salt, SALT_LEN, salt_hex);
    to_hex(w->nonce, NONCE_LEN, nonce_hex);

    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(o, "format", FILE_FORMAT);
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "hostname", cfg.hostname);
    cJSON_AddNumberToObject(o, "created", (double)now);
    cJSON_AddNumberToObject(o, "devices", (double)device_db_known_count());
    cJSON_AddNumberToObject(o, "links", (double)links_count());
    cJSON_AddNumberToObject(o, "icons", (double)icons_count());
    cJSON_AddStringToObject(o, "kdf", "pbkdf2-sha256");
    cJSON_AddNumberToObject(o, "iterations", KDF_ITERS);
    cJSON_AddStringToObject(o, "salt", salt_hex);
    cJSON_AddStringToObject(o, "nonce", nonce_hex);

    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const size_t len = strlen(text);
    if (len > HDR_MAX) {
        cJSON_free(text);
        return ESP_ERR_INVALID_SIZE;
    }

    /* The frame buffer is free until the first record. */
    uint8_t *p = w->frame;
    memcpy(p, FILE_MAGIC, FILE_MAGIC_LEN);
    p[FILE_MAGIC_LEN]     = (uint8_t)len;
    p[FILE_MAGIC_LEN + 1] = (uint8_t)(len >> 8);
    memcpy(p + FILE_MAGIC_LEN + 2, text, len);
    cJSON_free(text);

    const size_t total = FILE_MAGIC_LEN + 2 + len;
    mbedtls_sha256(p, total, w->aad, 0);
    return w->write(w->ctx, p, total);
}

esp_err_t backup_export(const char *passphrase, int64_t now, backup_write_fn write_fn,
                        void *ctx)
{
    if (!backup_passphrase_ok(passphrase) || write_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    writer_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ESP_ERR_NO_MEM;
    }
    w->write = write_fn;
    w->ctx   = ctx;
    mbedtls_gcm_init(&w->gcm);

    uint8_t salt[SALT_LEN];
    uint8_t key[KEY_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_fill_random(w->nonce, sizeof(w->nonce));

    const int64_t t0  = esp_log_timestamp();
    esp_err_t     err = derive_key(passphrase, salt, KDF_ITERS, key);
    if (err == ESP_OK &&
        mbedtls_gcm_setkey(&w->gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) != 0) {
        err = ESP_FAIL;
    }
    memset(key, 0, sizeof(key));

    if (err == ESP_OK) {
        err = write_prefix(w, salt, now);
    }
    for (size_t i = 0; err == ESP_OK && i < COUNT_OF(k_namespaces); i++) {
        err = export_namespace(w, k_namespaces[i]);
    }
    if (err == ESP_OK) {
        err = export_register(w);
    }
    if (err == ESP_OK) {
        err = export_icons(w);
    }
    if (err == ESP_OK) {
        w->pt[0] = REC_END;
        put_u32le(w->pt + 1, w->index);
        err = emit(w, 5);
    }

    const uint32_t records = w->index;
    mbedtls_gcm_free(&w->gcm);
    memset(w, 0, sizeof(*w));   /* the plaintext included the Wi-Fi password */
    free(w);

    if (err == ESP_OK) {
        record_backup_time(now);
        ESP_LOGI(TAG, "backup made: %u records in %u ms", (unsigned)records,
                 (unsigned)(esp_log_timestamp() - t0));
    } else {
        ESP_LOGE(TAG, "backup failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ------------------------------------------------------------------------- */
/* Restore: read, check, stage                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    backup_read_fn         fn;
    void                  *ctx;
    size_t                 total;       /* bytes consumed                    */

    mbedtls_gcm_context    gcm;
    uint8_t                nonce[NONCE_LEN];
    uint8_t                aad[AAD_LEN];

    const esp_partition_t *part;
    size_t                 stage_len;   /* bytes staged after the header     */
    size_t                 erased;      /* partition offset erased up to     */
    uint32_t               staged;      /* records staged                    */
    mbedtls_sha256_context sha;

    uint32_t               file_left;   /* bytes the open file still expects */

    char                   pass[BACKUP_PASS_MAX + 2];
    uint8_t                prefix[PREFIX_MAX];
    uint8_t                ct[REC_MAX + GCM_TAG_LEN];
    uint8_t                pt[REC_MAX];
} restore_t;

/*
 * Exactly len bytes. ESP_ERR_NOT_FOUND when the input ended before the first
 * of them - a clean end - and ESP_ERR_INVALID_CRC when it ended part-way.
 */
static esp_err_t read_exact(restore_t *s, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        const int n = s->fn(s->ctx, (uint8_t *)buf + got, len - got);
        if (n < 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (n == 0) {
            return got == 0 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_CRC;
        }
        got += (size_t)n;
        s->total += (size_t)n;
        if (s->total > BACKUP_MAX_BYTES + BACKUP_PASS_MAX + 1) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

/* The passphrase line. False when there is no newline where one must be. */
static bool read_passphrase(restore_t *s)
{
    for (size_t i = 0; i <= BACKUP_PASS_MAX; i++) {
        char c;
        if (read_exact(s, &c, 1) != ESP_OK) {
            return false;
        }
        if (c == '\n') {
            s->pass[i] = '\0';
            return true;
        }
        s->pass[i] = c;
    }
    return false;
}

static bool json_string(const cJSON *o, const char *name, char *out, size_t cap)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, name);
    if (!cJSON_IsString(j)) {
        return false;
    }
    snprintf(out, cap, "%s", j->valuestring);
    return true;
}

static double json_number(const cJSON *o, const char *name, double fallback)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsNumber(j) ? j->valuedouble : fallback;
}

/* Reads and checks the readable part. Fills info, the salt, the iterations. */
static esp_err_t read_prefix(restore_t *s, backup_info_t *info, uint8_t salt[SALT_LEN],
                             uint32_t *iters, char *err, size_t cap)
{
    uint8_t *p = s->prefix;
    if (read_exact(s, p, FILE_MAGIC_LEN + 2) != ESP_OK ||
        memcmp(p, FILE_MAGIC, FILE_MAGIC_LEN) != 0) {
        set_err(err, cap, "This is not a LANDA.SH backup file.");
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = (size_t)p[FILE_MAGIC_LEN] | ((size_t)p[FILE_MAGIC_LEN + 1] << 8);
    if (len == 0 || len > HDR_MAX || read_exact(s, p + FILE_MAGIC_LEN + 2, len) != ESP_OK) {
        set_err(err, cap, "This is not a LANDA.SH backup file.");
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_sha256(p, FILE_MAGIC_LEN + 2 + len, s->aad, 0);

    cJSON *o = cJSON_ParseWithLength((const char *)p + FILE_MAGIC_LEN + 2, len);
    if (o == NULL) {
        set_err(err, cap, "This is not a LANDA.SH backup file.");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t    e      = ESP_OK;
    const double format = json_number(o, "format", 0);
    const double it     = json_number(o, "iterations", 0);
    char         kdf[24]                      = "";
    char         salt_hex[SALT_LEN * 2 + 2]   = "";
    char         nonce_hex[NONCE_LEN * 2 + 2] = "";

    if (format > FILE_FORMAT) {
        set_err(err, cap, "This backup was made by a newer version of LANDA.SH. "
                          "Update this dongle first.");
        e = ESP_ERR_NOT_SUPPORTED;
    } else if (format != FILE_FORMAT || !json_string(o, "fw", info->fw, sizeof(info->fw)) ||
               !json_string(o, "hostname", info->hostname, sizeof(info->hostname)) ||
               !json_string(o, "kdf", kdf, sizeof(kdf)) ||
               !json_string(o, "salt", salt_hex, sizeof(salt_hex)) ||
               !json_string(o, "nonce", nonce_hex, sizeof(nonce_hex)) ||
               !from_hex(salt_hex, salt, SALT_LEN) ||
               !from_hex(nonce_hex, s->nonce, NONCE_LEN)) {
        set_err(err, cap, "This is not a LANDA.SH backup file.");
        e = ESP_ERR_INVALID_ARG;
    } else if (strcmp(kdf, "pbkdf2-sha256") != 0 || it < KDF_ITERS_MIN || it > KDF_ITERS_MAX) {
        set_err(err, cap, "This backup uses key settings this firmware does not know.");
        e = ESP_ERR_NOT_SUPPORTED;
    } else {
        *iters         = (uint32_t)it;
        info->created  = (int64_t)json_number(o, "created", 0);
        info->devices  = (uint32_t)json_number(o, "devices", 0);
        info->links    = (uint32_t)json_number(o, "links", 0);
        info->icons    = (uint32_t)json_number(o, "icons", 0);
    }
    cJSON_Delete(o);
    if (e != ESP_OK) {
        return e;
    }

    /*
     * Carried entries are only as new as the firmware that wrote them, and a
     * loader copes with older layouts, not newer ones. A development build
     * with no version to compare is let through: whoever runs one is testing.
     */
    int bv[3], rv[3];
    const char *running = esp_app_get_description()->version;
    if (ota_parse_version(info->fw, bv) && ota_parse_version(running, rv)) {
        for (int i = 0; i < 3; i++) {
            if (bv[i] != rv[i]) {
                if (bv[i] > rv[i]) {
                    if (err != NULL && cap > 0) {
                        snprintf(err, cap, "This backup was made by newer firmware (%s); "
                                           "update this dongle first.", info->fw);
                    }
                    return ESP_ERR_NOT_SUPPORTED;
                }
                break;
            }
        }
    }
    return ESP_OK;
}

/*
 * Checks a decrypted record is one this firmware understands and would write
 * back safely. Everything in it is authenticated, so this is about files from
 * a newer format or a hand-made one, not about tampering.
 */
static esp_err_t check_record(restore_t *s, const uint8_t *p, size_t len, uint32_t index)
{
    switch (p[0]) {
    case REC_NVS: {
        if (s->file_left != 0 || len < 4) {
            return ESP_ERR_INVALID_CRC;
        }
        size_t      n  = 1;
        const size_t nl = p[n++];
        if (nl < 1 || nl > 15 || n + nl + 2 > len) {
            return ESP_ERR_INVALID_CRC;
        }
        char ns[16];
        memcpy(ns, p + n, nl);
        ns[nl] = '\0';
        n += nl;
        const size_t kl = p[n++];
        if (kl < 1 || kl > 15 || n + kl + 1 > len) {
            return ESP_ERR_INVALID_CRC;
        }
        n += kl;
        const uint8_t type = p[n++];
        const size_t  vl   = len - n;
        if (!namespace_carried(ns)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (nvs_int_type(type)) {
            return vl == nvs_int_width(type) ? ESP_OK : ESP_ERR_INVALID_CRC;
        }
        if (type == NVS_TYPE_STR) {
            return memchr(p + n, '\0', vl) == NULL ? ESP_OK : ESP_ERR_INVALID_CRC;
        }
        return type == NVS_TYPE_BLOB ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    case REC_FILE: {
        if (s->file_left != 0 || len < 7) {
            return ESP_ERR_INVALID_CRC;
        }
        const size_t nl = p[1];
        if (len != 2 + nl + 4 || !file_name_ok((const char *)p + 2, nl)) {
            return ESP_ERR_INVALID_CRC;
        }
        const uint32_t size = get_u32le(p + 2 + nl);
        if (size > FILE_MAX_BYTES) {
            return ESP_ERR_INVALID_CRC;
        }
        s->file_left = size;
        return ESP_OK;
    }
    case REC_DATA:
        if (len < 2 || len - 1 > s->file_left) {
            return ESP_ERR_INVALID_CRC;
        }
        s->file_left -= (uint32_t)(len - 1);
        return ESP_OK;
    case REC_END:
        return len == 5 && s->file_left == 0 && get_u32le(p + 1) == index ? ESP_OK
                                                                           : ESP_ERR_INVALID_CRC;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/* Appends a record to the staging area, erasing sectors just ahead of it. */
static esp_err_t stage_put(restore_t *s, const uint8_t *p, size_t len)
{
    const uint8_t lenb[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
    const size_t  off     = STAGE_DATA_OFF + s->stage_len;
    const size_t  end     = off + 2 + len;

    if (end > s->part->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (s->erased < end) {
        esp_err_t err = esp_partition_erase_range(s->part, s->erased, SECTOR);
        if (err != ESP_OK) {
            return err;
        }
        s->erased += SECTOR;
    }
    esp_err_t err = esp_partition_write(s->part, off, lenb, 2);
    if (err == ESP_OK) {
        err = esp_partition_write(s->part, off + 2, p, len);
    }
    if (err == ESP_OK) {
        mbedtls_sha256_update(&s->sha, lenb, 2);
        mbedtls_sha256_update(&s->sha, p, len);
        s->stage_len += 2 + len;
        s->staged++;
    }
    return err;
}

static esp_err_t stage_finish(restore_t *s, const backup_info_t *info)
{
    stage_hdr_t *hdr = (stage_hdr_t *)s->ct;   /* free now; too big for the stack */
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, STAGE_MAGIC, sizeof(hdr->magic));
    hdr->data_len = (uint32_t)s->stage_len;
    hdr->records  = s->staged;
    mbedtls_sha256_finish(&s->sha, hdr->sha);
    memcpy(hdr->fw, info->fw, sizeof(hdr->fw));
    memcpy(hdr->hostname, info->hostname, sizeof(hdr->hostname));
    hdr->created = info->created;
    hdr->devices = info->devices;
    hdr->links   = info->links;
    hdr->icons   = info->icons;
    return esp_partition_write(s->part, 0, hdr, sizeof(*hdr));
}

/* Reads, checks and stages every record, up to and including the end record. */
static esp_err_t restore_records(restore_t *s, char *err, size_t cap)
{
    for (uint32_t index = 0;; index++) {
        if (index % BREATHE_EVERY == BREATHE_EVERY - 1) {
            breathe();
        }
        uint8_t   lenb[4];
        esp_err_t e = read_exact(s, lenb, sizeof(lenb));
        if (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_INVALID_CRC) {
            set_err(err, cap, "The backup is damaged or incomplete.");
            return ESP_ERR_INVALID_CRC;
        }
        if (e != ESP_OK) {
            return e;
        }
        const size_t len = get_u32le(lenb);
        if (len < 1 || len > REC_MAX) {
            set_err(err, cap, index == 0 ? "This is not a LANDA.SH backup file."
                                         : "The backup is damaged or incomplete.");
            return index == 0 ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_CRC;
        }
        e = read_exact(s, s->ct, len + GCM_TAG_LEN);
        if (e == ESP_ERR_NOT_FOUND || e == ESP_ERR_INVALID_CRC) {
            set_err(err, cap, "The backup is damaged or incomplete.");
            return ESP_ERR_INVALID_CRC;
        }
        if (e != ESP_OK) {
            return e;
        }

        uint8_t iv[IV_LEN];
        make_iv(s->nonce, index, iv);
        if (mbedtls_gcm_auth_decrypt(&s->gcm, len, iv, IV_LEN, s->aad, AAD_LEN, s->ct + len,
                                     GCM_TAG_LEN, s->ct, s->pt) != 0) {
            /* The first record is the one the passphrase has to open. */
            if (index == 0) {
                set_err(err, cap, "Wrong passphrase.");
                return ESP_ERR_INVALID_MAC;
            }
            set_err(err, cap, "The backup is damaged or incomplete.");
            return ESP_ERR_INVALID_CRC;
        }

        e = check_record(s, s->pt, len, index);
        if (e == ESP_ERR_NOT_SUPPORTED) {
            set_err(err, cap, "This backup holds something this firmware does not know. "
                              "Update this dongle first.");
            return e;
        }
        if (e != ESP_OK) {
            set_err(err, cap, "The backup is damaged or incomplete.");
            return e;
        }

        if (s->pt[0] == REC_END) {
            uint8_t extra;
            if (read_exact(s, &extra, 1) != ESP_ERR_NOT_FOUND) {
                set_err(err, cap, "The backup is damaged or incomplete.");
                return ESP_ERR_INVALID_CRC;
            }
            return ESP_OK;
        }

        e = stage_put(s, s->pt, len);
        if (e == ESP_ERR_INVALID_SIZE) {
            set_err(err, cap, "The backup is too large to restore.");
            return e;
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "staging write failed: %s", esp_err_to_name(e));
            set_err(err, cap, "Could not write the backup to flash.");
            return ESP_FAIL;
        }
    }
}

esp_err_t backup_restore(backup_read_fn read_fn, void *ctx, backup_info_t *out_info, char *err,
                         size_t err_cap)
{
    set_err(err, err_cap, "");
    if (read_fn == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));

    restore_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_err(err, err_cap, "Out of memory.");
        return ESP_ERR_NO_MEM;
    }
    s->fn  = read_fn;
    s->ctx = ctx;
    mbedtls_gcm_init(&s->gcm);
    mbedtls_sha256_init(&s->sha);

    uint8_t   salt[SALT_LEN];
    uint32_t  iters = 0;
    esp_err_t e     = ESP_OK;

    if (!read_passphrase(s) || !backup_passphrase_ok(s->pass)) {
        set_err(err, err_cap, "The passphrase is missing or not 8 to 128 characters.");
        e = ESP_ERR_INVALID_ARG;
    }
    if (e == ESP_OK) {
        e = read_prefix(s, out_info, salt, &iters, err, err_cap);
    }

    /* Before the slow part, so a busy slot is reported at once. */
    if (e == ESP_OK) {
        s->part = ota_borrow_slot();
        if (s->part == NULL) {
            set_err(err, err_cap, "A firmware update is running or has only just been "
                                  "installed. Try again in a few minutes.");
            e = ESP_ERR_INVALID_STATE;
        }
    }
    /* Nothing staged survives from before: the header sector goes first. */
    if (e == ESP_OK) {
        e = esp_partition_erase_range(s->part, 0, SECTOR);
        s->erased = STAGE_DATA_OFF;
        if (e != ESP_OK) {
            set_err(err, err_cap, "Could not write the backup to flash.");
            e = ESP_FAIL;
        }
    }

    if (e == ESP_OK) {
        uint8_t key[KEY_LEN];
        e = derive_key(s->pass, salt, iters, key);
        if (e == ESP_OK &&
            mbedtls_gcm_setkey(&s->gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) != 0) {
            e = ESP_FAIL;
        }
        memset(key, 0, sizeof(key));
        if (e != ESP_OK) {
            set_err(err, err_cap, "Could not derive the key.");
        }
    }

    if (e == ESP_OK) {
        mbedtls_sha256_starts(&s->sha, 0);
        e = restore_records(s, err, err_cap);
    }
    if (e == ESP_OK) {
        e = stage_finish(s, out_info);
        if (e != ESP_OK) {
            set_err(err, err_cap, "Could not write the backup to flash.");
            e = ESP_FAIL;
        }
    }
    if (e == ESP_ERR_TIMEOUT) {
        set_err(err, err_cap, "The upload stopped part-way.");
    } else if (e == ESP_ERR_INVALID_SIZE && err != NULL && err[0] == '\0') {
        set_err(err, err_cap, "The backup is too large to restore.");
    }

    if (e == ESP_OK) {
        ESP_LOGW(TAG, "backup of %s (%s) staged: %u records, %u bytes; applies at the next boot",
                 out_info->hostname, out_info->fw, (unsigned)s->staged,
                 (unsigned)s->stage_len);
        /* The slot stays lent: the caller reboots, and that gives it back. */
    } else {
        if (s->part != NULL) {
            ota_return_slot();
        }
        ESP_LOGW(TAG, "restore refused: %s (%s)", esp_err_to_name(e), err ? err : "");
    }

    mbedtls_gcm_free(&s->gcm);
    mbedtls_sha256_free(&s->sha);
    memset(s, 0, sizeof(*s));
    free(s);
    return e;
}

/* ------------------------------------------------------------------------- */
/* Restore: apply at boot                                                    */
/* ------------------------------------------------------------------------- */

static void erase_namespace(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_erase_all(h) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static esp_err_t write_nvs_entry(nvs_handle_t h, const char *key, uint8_t type,
                                 const uint8_t *v, size_t vl)
{
    uint64_t x = 0;
    if (nvs_int_type(type)) {
        for (size_t i = 0; i < vl; i++) {
            x |= (uint64_t)v[i] << (8 * i);
        }
    }
    switch (type) {
    case NVS_TYPE_U8:   return nvs_set_u8(h, key, (uint8_t)x);
    case NVS_TYPE_I8:   return nvs_set_i8(h, key, (int8_t)(uint8_t)x);
    case NVS_TYPE_U16:  return nvs_set_u16(h, key, (uint16_t)x);
    case NVS_TYPE_I16:  return nvs_set_i16(h, key, (int16_t)(uint16_t)x);
    case NVS_TYPE_U32:  return nvs_set_u32(h, key, (uint32_t)x);
    case NVS_TYPE_I32:  return nvs_set_i32(h, key, (int32_t)(uint32_t)x);
    case NVS_TYPE_U64:  return nvs_set_u64(h, key, x);
    case NVS_TYPE_I64:  return nvs_set_i64(h, key, (int64_t)x);
    case NVS_TYPE_STR:  return nvs_set_str(h, key, (const char *)v);   /* NUL-ended by caller */
    case NVS_TYPE_BLOB: return nvs_set_blob(h, key, v, vl);
    default:            return ESP_ERR_NOT_SUPPORTED;
    }
}

typedef struct {
    nvs_handle_t h;
    char         ns[16];
    FILE        *f;
    uint32_t     file_left;
    bool         storage;
    unsigned     entries;
    unsigned     files;
    unsigned     failures;
} apply_t;

/* One staged record. buf has a spare byte past len for a string's NUL. */
static void apply_record(apply_t *a, uint8_t *p, size_t len)
{
    switch (p[0]) {
    case REC_NVS: {
        size_t       n  = 1;
        const size_t nl = p[n++];
        char         ns[16];
        memcpy(ns, p + n, nl);
        ns[nl] = '\0';
        n += nl;
        const size_t kl = p[n++];
        char         key[16];
        memcpy(key, p + n, kl);
        key[kl] = '\0';
        n += kl;
        const uint8_t type = p[n++];
        p[len]             = '\0';

        if (strcmp(ns, a->ns) != 0) {
            if (a->ns[0] != '\0') {
                nvs_commit(a->h);
                nvs_close(a->h);
                a->ns[0] = '\0';
            }
            if (nvs_open(ns, NVS_READWRITE, &a->h) != ESP_OK) {
                a->failures++;
                return;
            }
            snprintf(a->ns, sizeof(a->ns), "%s", ns);
        }
        const esp_err_t err = write_nvs_entry(a->h, key, type, p + n, len - n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot restore %s/%s: %s", ns, key, esp_err_to_name(err));
            a->failures++;
        } else {
            a->entries++;
        }
        return;
    }
    case REC_FILE: {
        const size_t nl = p[1];
        char         path[48];
        snprintf(path, sizeof(path), NETDASH_STORAGE_BASE "/%.*s", (int)nl, (const char *)p + 2);
        a->file_left = get_u32le(p + 2 + nl);
        if (!a->storage) {
            return;
        }
        a->f = fopen(path, "wb");
        if (a->f == NULL) {
            ESP_LOGE(TAG, "cannot create %s", path);
            a->failures++;
        } else {
            a->files++;
        }
        break;
    }
    case REC_DATA:
        if (a->f != NULL && fwrite(p + 1, 1, len - 1, a->f) != len - 1) {
            ESP_LOGE(TAG, "file write failed; storage full?");
            a->failures++;
        }
        a->file_left -= (uint32_t)(len - 1);
        break;
    default:
        return;
    }
    if (a->file_left == 0 && a->f != NULL) {
        fclose(a->f);
        a->f = NULL;
    }
}

static void record_restore(const stage_hdr_t *hdr)
{
    nvs_handle_t h;
    if (nvs_open(STATUS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    status_from_t from = {.version = STATUS_FROM_VER};
    memcpy(from.fw, hdr->fw, sizeof(from.fw));
    memcpy(from.hostname, hdr->hostname, sizeof(from.hostname));
    from.created = hdr->created;
    from.devices = hdr->devices;
    from.links   = hdr->links;
    from.icons   = hdr->icons;

    /* The file is a backup of exactly what is now here. */
    if (hdr->created > 0) {
        nvs_set_i64(h, STATUS_LAST, hdr->created);
    } else {
        nvs_erase_key(h, STATUS_LAST);
    }
    nvs_set_blob(h, STATUS_FROM, &from, sizeof(from));
    nvs_commit(h);
    nvs_close(h);
}

void backup_apply_pending(void)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return;
    }
    stage_hdr_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        memcmp(hdr.magic, STAGE_MAGIC, sizeof(hdr.magic)) != 0) {
        return;   /* the usual case: an app image, or nothing */
    }

    uint8_t *buf = malloc(REC_MAX + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory to apply the staged restore; left for the next boot");
        return;
    }

    /* Check the staged bytes before wiping anything for them. */
    bool ok = hdr.data_len <= part->size - STAGE_DATA_OFF;
    if (ok) {
        mbedtls_sha256_context sha;
        uint8_t                digest[32];
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        for (size_t off = 0; ok && off < hdr.data_len; off += REC_MAX) {
            const size_t n = hdr.data_len - off < REC_MAX ? hdr.data_len - off : REC_MAX;
            ok = esp_partition_read(part, STAGE_DATA_OFF + off, buf, n) == ESP_OK;
            mbedtls_sha256_update(&sha, buf, n);
        }
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_free(&sha);
        ok = ok && memcmp(digest, hdr.sha, sizeof(digest)) == 0;
    }

    if (!ok) {
        ESP_LOGE(TAG, "staged restore is damaged; discarding it");
    } else {
        ESP_LOGW(TAG, "applying the backup of %.*s (%.*s): %u records",
                 (int)sizeof(hdr.hostname), hdr.hostname, (int)sizeof(hdr.fw), hdr.fw,
                 (unsigned)hdr.records);

        for (size_t i = 0; i < COUNT_OF(k_namespaces); i++) {
            erase_namespace(k_namespaces[i]);
        }
        for (size_t i = 0; i < COUNT_OF(k_legacy_namespaces); i++) {
            erase_namespace(k_legacy_namespaces[i]);
        }

        apply_t a = {.storage = icons_storage_claim_empty() == ESP_OK};
        if (!a.storage) {
            ESP_LOGE(TAG, "storage partition unavailable; devices and icons not restored");
        }

        for (size_t off = 0; off + 2 <= hdr.data_len;) {
            uint8_t lenb[2];
            if (esp_partition_read(part, STAGE_DATA_OFF + off, lenb, 2) != ESP_OK) {
                a.failures++;
                break;
            }
            const size_t len = (size_t)lenb[0] | ((size_t)lenb[1] << 8);
            if (len < 1 || len > REC_MAX || off + 2 + len > hdr.data_len ||
                esp_partition_read(part, STAGE_DATA_OFF + off + 2, buf, len) != ESP_OK) {
                a.failures++;
                break;
            }
            apply_record(&a, buf, len);
            off += 2 + len;
        }

        if (a.f != NULL) {
            fclose(a.f);
        }
        if (a.ns[0] != '\0') {
            nvs_commit(a.h);
            nvs_close(a.h);
        }
        if (a.storage) {
            icons_storage_release();
        }
        record_restore(&hdr);

        if (a.failures != 0) {
            ESP_LOGE(TAG, "restore applied with %u failure(s): %u entries, %u files",
                     a.failures, a.entries, a.files);
        } else {
            ESP_LOGW(TAG, "restore applied: %u entries, %u files", a.entries, a.files);
        }
    }

    /*
     * Done either way. A failed apply is not retried: every boot would fail
     * the same way. The staged records stay in the slot until the next update
     * overwrites them; they hold nothing that is not now in NVS as well.
     */
    esp_partition_erase_range(part, 0, SECTOR);
    free(buf);
}
