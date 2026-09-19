/*
 * NetDash per-device notes and the secret vault. See notes.h for the threat
 * model, which is worth reading before trusting this with anything.
 *
 * Both stores are keyed by the 12-hex-lowercase MAC, the same key scheme the
 * device table uses in namespace "dev". Which MACs have an entry is mirrored
 * into two small in-RAM sets so the device list can show a "has a note" marker
 * without an NVS lookup per row per poll.
 */
#include "notes.h"

#include <stdlib.h>
#include <string.h>

#include "device_db.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "notes";

#define NOTE_NVS_NS  "note"
#define SEC_NVS_NS   "sec"
#define VAULT_NVS_NS "vault"
#define VAULT_KEY    "meta"

#define NOTE_BLOB_VERSION  1
#define SEC_BLOB_VERSION   1
#define VAULT_META_VERSION 1

/*
 * PBKDF2 rounds. This is the entire cost of guessing the passphrase from a
 * flash dump, so it wants to be as high as the unlock delay tolerates. The
 * count is stored in the vault metadata rather than assumed, so raising it
 * later does not lock anyone out of an existing vault.
 */
#define VAULT_PBKDF2_ITERS 40000

#define VAULT_SALT_LEN     16
#define VAULT_KEY_LEN      32
#define VAULT_IV_LEN       12
#define VAULT_TAG_LEN      16
#define VAULT_VERIFY_LABEL "netdash-vault-v1"

typedef struct __attribute__((packed)) {
    uint8_t version;
    char    text[NETDASH_NOTE_MAX];
} note_blob_t;

/*
 * Fixed size on purpose: a blob sized to its contents would leak the length of
 * every secret to anyone who could read the flash.
 */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  iv[VAULT_IV_LEN];
    uint8_t  tag[VAULT_TAG_LEN];
    uint16_t len;                       /* plaintext length, excluding NUL */
    uint8_t  ct[NETDASH_SECRET_MAX];
} sec_blob_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  salt[VAULT_SALT_LEN];
    uint32_t iters;
    uint8_t  verifier[32];
} vault_meta_t;

/* In-RAM mirrors of which MACs have an entry. */
static uint8_t s_note_macs[NETDASH_MAX_DEVICES][6];
static size_t  s_note_count;
static uint8_t s_sec_macs[NETDASH_MAX_DEVICES][6];
static size_t  s_sec_count;

/* Live vault session. s_key is only ever populated while unlocked. */
static uint8_t  s_key[VAULT_KEY_LEN];
static bool     s_unlocked;
static char     s_token[NETDASH_VAULT_TOKEN_LEN];
static int64_t  s_last_use_us;

static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
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

static void mac_to_key(const uint8_t mac[6], char key[13])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        key[i * 2]     = hex[(mac[i] >> 4) & 0x0f];
        key[i * 2 + 1] = hex[mac[i] & 0x0f];
    }
    key[12] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool key_to_mac(const char *key, uint8_t mac[6])
{
    if (key == NULL || strlen(key) != 12) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int hi = hex_val(key[i * 2]);
        int lo = hex_val(key[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Does not branch on the contents, so a token cannot be guessed a byte at a
   time by timing the comparison. */
static bool const_time_eq(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t                 d = 0;

    for (size_t i = 0; i < len; i++) {
        d |= (uint8_t)(x[i] ^ y[i]);
    }
    return d == 0;
}

/* Index set maintenance. Caller holds the lock. */
static bool set_has(const uint8_t set[][6], size_t count, const uint8_t mac[6])
{
    for (size_t i = 0; i < count; i++) {
        if (memcmp(set[i], mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void set_add(uint8_t set[][6], size_t *count, const uint8_t mac[6])
{
    if (set_has((const uint8_t (*)[6])set, *count, mac) || *count >= NETDASH_MAX_DEVICES) {
        return;
    }
    memcpy(set[(*count)++], mac, 6);
}

static void set_remove(uint8_t set[][6], size_t *count, const uint8_t mac[6])
{
    for (size_t i = 0; i < *count; i++) {
        if (memcmp(set[i], mac, 6) == 0) {
            memmove(set[i], set[i + 1], 6 * (*count - i - 1));
            (*count)--;
            return;
        }
    }
}

/* Fills a set from every well-formed key in a namespace. Caller holds the lock. */
static void index_namespace(const char *ns, uint8_t set[][6], size_t *count)
{
    *count = 0;

    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    nvs_iterator_t it   = NULL;
    esp_err_t      fres = nvs_entry_find_in_handle(h, NVS_TYPE_BLOB, &it);
    while (fres == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        uint8_t mac[6];
        if (key_to_mac(info.key, mac)) {
            set_add(set, count, mac);
        }
        fres = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
}

static esp_err_t blob_write(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t blob_read(const char *ns, const char *key, void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = len;
    err         = nvs_get_blob(h, key, data, &size);
    nvs_close(h);
    if (err == ESP_OK && size != len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t blob_erase(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t notes_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock();
    index_namespace(NOTE_NVS_NS, s_note_macs, &s_note_count);
    index_namespace(SEC_NVS_NS, s_sec_macs, &s_sec_count);
    const size_t notes = s_note_count;
    const size_t secs  = s_sec_count;
    unlock();

    ESP_LOGI(TAG, "%u note(s), %u secret(s), vault %s", (unsigned)notes, (unsigned)secs,
             vault_configured() ? "configured" : "not set up");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Plain text notes                                                          */
/* ------------------------------------------------------------------------- */

esp_err_t notes_set(const uint8_t mac[6], const char *text)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[13];
    mac_to_key(mac, key);

    if (text == NULL || text[0] == '\0') {
        esp_err_t err = blob_erase(NOTE_NVS_NS, key);
        if (err == ESP_OK) {
            lock();
            set_remove(s_note_macs, &s_note_count, mac);
            unlock();
        }
        return err;
    }

    note_blob_t blob = {0};
    blob.version     = NOTE_BLOB_VERSION;
    strncpy(blob.text, text, sizeof(blob.text) - 1);

    esp_err_t err = blob_write(NOTE_NVS_NS, key, &blob, sizeof(blob));
    if (err == ESP_OK) {
        lock();
        set_add(s_note_macs, &s_note_count, mac);
        unlock();
    } else {
        ESP_LOGW(TAG, "note not saved for %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

bool notes_get(const uint8_t mac[6], char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (mac == NULL || !notes_exists(mac)) {
        return false;
    }

    char key[13];
    mac_to_key(mac, key);

    note_blob_t blob;
    if (blob_read(NOTE_NVS_NS, key, &blob, sizeof(blob)) != ESP_OK ||
        blob.version != NOTE_BLOB_VERSION) {
        return false;
    }
    blob.text[sizeof(blob.text) - 1] = '\0';
    strncpy(out, blob.text, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

bool notes_exists(const uint8_t mac[6])
{
    if (mac == NULL) {
        return false;
    }
    lock();
    const bool has = set_has((const uint8_t (*)[6])s_note_macs, s_note_count, mac);
    unlock();
    return has;
}

esp_err_t notes_forget_device(const uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[13];
    mac_to_key(mac, key);

    esp_err_t e1 = blob_erase(NOTE_NVS_NS, key);
    esp_err_t e2 = blob_erase(SEC_NVS_NS, key);

    lock();
    set_remove(s_note_macs, &s_note_count, mac);
    set_remove(s_sec_macs, &s_sec_count, mac);
    unlock();

    return e1 != ESP_OK ? e1 : e2;
}

/* ------------------------------------------------------------------------- */
/* Vault internals                                                           */
/* ------------------------------------------------------------------------- */

static bool meta_read(vault_meta_t *meta)
{
    return blob_read(VAULT_NVS_NS, VAULT_KEY, meta, sizeof(*meta)) == ESP_OK &&
           meta->version == VAULT_META_VERSION;
}

static esp_err_t derive_key(const char *pass, const uint8_t *salt, uint32_t iters,
                            uint8_t out[VAULT_KEY_LEN])
{
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pass,
                                                 strlen(pass), salt, VAULT_SALT_LEN, iters,
                                                 VAULT_KEY_LEN, out);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

/* SHA-256 over the key and a fixed label. Proves the passphrase without
   storing anything that could be used as the key itself. */
static void make_verifier(const uint8_t key[VAULT_KEY_LEN], uint8_t out[32])
{
    uint8_t buf[VAULT_KEY_LEN + sizeof(VAULT_VERIFY_LABEL) - 1];

    memcpy(buf, key, VAULT_KEY_LEN);
    memcpy(buf + VAULT_KEY_LEN, VAULT_VERIFY_LABEL, sizeof(VAULT_VERIFY_LABEL) - 1);
    mbedtls_sha256(buf, sizeof(buf), out, 0);
    memset(buf, 0, sizeof(buf));
}

/*
 * The MAC goes in as additional authenticated data, so a ciphertext lifted out
 * of one device's slot and dropped into another's fails its tag rather than
 * quietly decrypting under the wrong name.
 */
static esp_err_t gcm_encrypt(const uint8_t key[VAULT_KEY_LEN], const uint8_t mac[6],
                             const uint8_t *iv, const uint8_t *pt, size_t len,
                             uint8_t *ct, uint8_t *tag)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, VAULT_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, iv, VAULT_IV_LEN, mac, 6,
                                       pt, ct, VAULT_TAG_LEN, tag);
    }
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t gcm_decrypt(const uint8_t key[VAULT_KEY_LEN], const uint8_t mac[6],
                             const uint8_t *iv, const uint8_t *tag, const uint8_t *ct,
                             size_t len, uint8_t *pt)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, VAULT_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, len, iv, VAULT_IV_LEN, mac, 6, tag, VAULT_TAG_LEN,
                                      ct, pt);
    }
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? ESP_OK : ESP_ERR_INVALID_MAC;
}

/* Relocks when the session has gone unused. Caller holds the lock. */
static void check_idle_locked(void)
{
    if (!s_unlocked) {
        return;
    }
    const int64_t idle_us = esp_timer_get_time() - s_last_use_us;
    if (idle_us > (int64_t)NETDASH_VAULT_IDLE_S * 1000000) {
        memset(s_key, 0, sizeof(s_key));
        memset(s_token, 0, sizeof(s_token));
        s_unlocked = false;
        ESP_LOGI(TAG, "vault relocked after %d s idle", NETDASH_VAULT_IDLE_S);
    }
}

/* Copies the session key out for use. False when locked. Takes the lock. */
static bool session_key(uint8_t out[VAULT_KEY_LEN])
{
    lock();
    check_idle_locked();
    const bool ok = s_unlocked;
    if (ok) {
        memcpy(out, s_key, VAULT_KEY_LEN);
        s_last_use_us = esp_timer_get_time();
    }
    unlock();
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Vault                                                                     */
/* ------------------------------------------------------------------------- */

bool vault_configured(void)
{
    vault_meta_t meta;
    return meta_read(&meta);
}

bool vault_unlocked(void)
{
    lock();
    check_idle_locked();
    const bool ok = s_unlocked;
    unlock();
    return ok;
}

uint32_t vault_idle_remaining(void)
{
    lock();
    check_idle_locked();
    uint32_t left = 0;
    if (s_unlocked) {
        const int64_t idle_s = (esp_timer_get_time() - s_last_use_us) / 1000000;
        left = idle_s >= NETDASH_VAULT_IDLE_S ? 0 : (uint32_t)(NETDASH_VAULT_IDLE_S - idle_s);
    }
    unlock();
    return left;
}

esp_err_t vault_unlock(const char *pass, char *out_token, size_t cap)
{
    if (pass == NULL || out_token == NULL || cap < NETDASH_VAULT_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    vault_meta_t meta;
    if (!meta_read(&meta)) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t key[VAULT_KEY_LEN];
    if (derive_key(pass, meta.salt, meta.iters, key) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t verifier[32];
    make_verifier(key, verifier);
    if (!const_time_eq(verifier, meta.verifier, sizeof(verifier))) {
        memset(key, 0, sizeof(key));
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t           raw[16];
    static const char hex[] = "0123456789abcdef";
    esp_fill_random(raw, sizeof(raw));

    lock();
    memcpy(s_key, key, sizeof(s_key));
    for (size_t i = 0; i < sizeof(raw); i++) {
        s_token[i * 2]     = hex[(raw[i] >> 4) & 0x0f];
        s_token[i * 2 + 1] = hex[raw[i] & 0x0f];
    }
    s_token[NETDASH_VAULT_TOKEN_LEN - 1] = '\0';
    s_unlocked                           = true;
    s_last_use_us                        = esp_timer_get_time();
    memcpy(out_token, s_token, NETDASH_VAULT_TOKEN_LEN);
    unlock();

    memset(key, 0, sizeof(key));
    ESP_LOGI(TAG, "vault unlocked");
    return ESP_OK;
}

void vault_lock(void)
{
    lock();
    const bool was = s_unlocked;
    memset(s_key, 0, sizeof(s_key));
    memset(s_token, 0, sizeof(s_token));
    s_unlocked = false;
    unlock();

    if (was) {
        ESP_LOGI(TAG, "vault locked");
    }
}

bool vault_token_valid(const char *token)
{
    if (token == NULL || strlen(token) != NETDASH_VAULT_TOKEN_LEN - 1) {
        return false;
    }

    lock();
    check_idle_locked();
    bool ok = false;
    if (s_unlocked) {
        ok = const_time_eq(token, s_token, NETDASH_VAULT_TOKEN_LEN - 1);
        if (ok) {
            s_last_use_us = esp_timer_get_time();
        }
    }
    unlock();
    return ok;
}

esp_err_t vault_set_passphrase(const char *old_pass, const char *new_pass,
                               char *out_token, size_t cap)
{
    if (new_pass == NULL || strlen(new_pass) < NETDASH_VAULT_PASS_MIN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_token == NULL || cap < NETDASH_VAULT_TOKEN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    vault_meta_t meta;
    const bool   exists = meta_read(&meta);

    uint8_t old_key[VAULT_KEY_LEN] = {0};
    if (exists) {
        if (old_pass == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        if (derive_key(old_pass, meta.salt, meta.iters, old_key) != ESP_OK) {
            return ESP_FAIL;
        }
        uint8_t verifier[32];
        make_verifier(old_key, verifier);
        if (!const_time_eq(verifier, meta.verifier, sizeof(verifier))) {
            memset(old_key, 0, sizeof(old_key));
            return ESP_ERR_INVALID_STATE;
        }
    }

    vault_meta_t fresh = {0};
    fresh.version      = VAULT_META_VERSION;
    fresh.iters        = VAULT_PBKDF2_ITERS;
    esp_fill_random(fresh.salt, sizeof(fresh.salt));

    uint8_t new_key[VAULT_KEY_LEN];
    if (derive_key(new_pass, fresh.salt, fresh.iters, new_key) != ESP_OK) {
        memset(old_key, 0, sizeof(old_key));
        return ESP_FAIL;
    }
    make_verifier(new_key, fresh.verifier);

    /*
     * Re-encrypt everything already stored. Every secret is decrypted into a
     * scratch buffer first, so a single unreadable blob aborts the rotation
     * before anything has been overwritten with a key the rest cannot open.
     */
    esp_err_t err = ESP_OK;

    lock();
    const size_t n_sec = s_sec_count;
    static uint8_t macs[NETDASH_MAX_DEVICES][6];
    memcpy(macs, s_sec_macs, sizeof(macs));
    unlock();

    if (exists && n_sec > 0) {
        char *scratch = calloc(n_sec, NETDASH_SECRET_MAX);
        if (scratch == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            for (size_t i = 0; i < n_sec && err == ESP_OK; i++) {
                char key_str[13];
                mac_to_key(macs[i], key_str);

                sec_blob_t blob;
                if (blob_read(SEC_NVS_NS, key_str, &blob, sizeof(blob)) != ESP_OK ||
                    blob.version != SEC_BLOB_VERSION || blob.len >= NETDASH_SECRET_MAX) {
                    err = ESP_ERR_INVALID_SIZE;
                    break;
                }
                uint8_t *dst = (uint8_t *)(scratch + i * NETDASH_SECRET_MAX);
                err = gcm_decrypt(old_key, macs[i], blob.iv, blob.tag, blob.ct, blob.len, dst);
                if (err == ESP_OK) {
                    dst[blob.len] = '\0';
                }
            }

            for (size_t i = 0; i < n_sec && err == ESP_OK; i++) {
                const char *pt  = scratch + i * NETDASH_SECRET_MAX;
                const size_t len = strlen(pt);

                sec_blob_t blob = {0};
                blob.version    = SEC_BLOB_VERSION;
                blob.len        = (uint16_t)len;
                esp_fill_random(blob.iv, sizeof(blob.iv));
                err = gcm_encrypt(new_key, macs[i], blob.iv, (const uint8_t *)pt, len, blob.ct,
                                  blob.tag);
                if (err == ESP_OK) {
                    char key_str[13];
                    mac_to_key(macs[i], key_str);
                    err = blob_write(SEC_NVS_NS, key_str, &blob, sizeof(blob));
                }
                memset(&blob, 0, sizeof(blob));
            }

            memset(scratch, 0, n_sec * NETDASH_SECRET_MAX);
            free(scratch);
        }
    }

    if (err == ESP_OK) {
        err = blob_write(VAULT_NVS_NS, VAULT_KEY, &fresh, sizeof(fresh));
    }

    if (err == ESP_OK) {
        /* Leave the caller with an unlocked vault: they just proved they own it. */
        uint8_t           raw[16];
        static const char hex[] = "0123456789abcdef";
        esp_fill_random(raw, sizeof(raw));

        lock();
        memcpy(s_key, new_key, sizeof(s_key));
        for (size_t i = 0; i < sizeof(raw); i++) {
            s_token[i * 2]     = hex[(raw[i] >> 4) & 0x0f];
            s_token[i * 2 + 1] = hex[raw[i] & 0x0f];
        }
        s_token[NETDASH_VAULT_TOKEN_LEN - 1] = '\0';
        s_unlocked                           = true;
        s_last_use_us                        = esp_timer_get_time();
        memcpy(out_token, s_token, NETDASH_VAULT_TOKEN_LEN);
        unlock();

        ESP_LOGI(TAG, "vault passphrase %s (%u secret(s) re-encrypted)",
                 exists ? "changed" : "set", (unsigned)(exists ? n_sec : 0));
    } else {
        ESP_LOGE(TAG, "passphrase change failed: %s", esp_err_to_name(err));
    }

    memset(old_key, 0, sizeof(old_key));
    memset(new_key, 0, sizeof(new_key));
    return err;
}

esp_err_t vault_reset(void)
{
    lock();
    const size_t n = s_sec_count;
    static uint8_t macs[NETDASH_MAX_DEVICES][6];
    memcpy(macs, s_sec_macs, sizeof(macs));
    unlock();

    for (size_t i = 0; i < n; i++) {
        char key[13];
        mac_to_key(macs[i], key);
        blob_erase(SEC_NVS_NS, key);
    }
    esp_err_t err = blob_erase(VAULT_NVS_NS, VAULT_KEY);

    lock();
    s_sec_count = 0;
    memset(s_sec_macs, 0, sizeof(s_sec_macs));
    memset(s_key, 0, sizeof(s_key));
    memset(s_token, 0, sizeof(s_token));
    s_unlocked = false;
    unlock();

    ESP_LOGW(TAG, "vault reset, %u secret(s) destroyed", (unsigned)n);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Secrets                                                                   */
/* ------------------------------------------------------------------------- */

esp_err_t secret_set(const uint8_t mac[6], const char *text)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char key_str[13];
    mac_to_key(mac, key_str);

    if (text == NULL || text[0] == '\0') {
        esp_err_t err = blob_erase(SEC_NVS_NS, key_str);
        if (err == ESP_OK) {
            lock();
            set_remove(s_sec_macs, &s_sec_count, mac);
            unlock();
        }
        return err;
    }

    const size_t len = strlen(text);
    if (len >= NETDASH_SECRET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t key[VAULT_KEY_LEN];
    if (!session_key(key)) {
        return ESP_ERR_INVALID_STATE;
    }

    sec_blob_t blob = {0};
    blob.version    = SEC_BLOB_VERSION;
    blob.len        = (uint16_t)len;
    esp_fill_random(blob.iv, sizeof(blob.iv));

    esp_err_t err =
        gcm_encrypt(key, mac, blob.iv, (const uint8_t *)text, len, blob.ct, blob.tag);
    memset(key, 0, sizeof(key));

    if (err == ESP_OK) {
        err = blob_write(SEC_NVS_NS, key_str, &blob, sizeof(blob));
    }
    memset(&blob, 0, sizeof(blob));

    if (err == ESP_OK) {
        lock();
        set_add(s_sec_macs, &s_sec_count, mac);
        unlock();
    } else {
        ESP_LOGW(TAG, "secret not saved for %s: %s", key_str, esp_err_to_name(err));
    }
    return err;
}

esp_err_t secret_get(const uint8_t mac[6], char *out, size_t cap)
{
    if (mac == NULL || out == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!secret_exists(mac)) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t key[VAULT_KEY_LEN];
    if (!session_key(key)) {
        return ESP_ERR_INVALID_STATE;
    }

    char key_str[13];
    mac_to_key(mac, key_str);

    sec_blob_t blob;
    esp_err_t  err = blob_read(SEC_NVS_NS, key_str, &blob, sizeof(blob));
    if (err != ESP_OK) {
        memset(key, 0, sizeof(key));
        return err;
    }
    if (blob.version != SEC_BLOB_VERSION || blob.len >= NETDASH_SECRET_MAX) {
        memset(key, 0, sizeof(key));
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t len = blob.len;
    uint8_t      pt[NETDASH_SECRET_MAX];

    err = gcm_decrypt(key, mac, blob.iv, blob.tag, blob.ct, len, pt);
    memset(key, 0, sizeof(key));
    memset(&blob, 0, sizeof(blob));

    if (err == ESP_OK) {
        pt[len] = '\0';
        strncpy(out, (const char *)pt, cap - 1);
        out[cap - 1] = '\0';
    }
    memset(pt, 0, sizeof(pt));
    return err;
}

bool secret_exists(const uint8_t mac[6])
{
    if (mac == NULL) {
        return false;
    }
    lock();
    const bool has = set_has((const uint8_t (*)[6])s_sec_macs, s_sec_count, mac);
    unlock();
    return has;
}

size_t secret_count(void)
{
    lock();
    const size_t n = s_sec_count;
    unlock();
    return n;
}
