/*
 * NetDash link notes and the secret vault. See notes.h for the threat model,
 * which is worth reading before trusting this with anything.
 *
 * Both stores are keyed by the link id in decimal. Which ids have an entry is
 * mirrored into two small in-RAM sets so the dashboard poll can flag them
 * without an NVS lookup per tile.
 */
#include "notes.h"

#include <stdlib.h>
#include <string.h>

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

#define LNOTE_NVS_NS "lnote"
#define LSEC_NVS_NS  "lsec"
#define VAULT_NVS_NS "vault"
#define VAULT_KEY    "meta"

/* Device secrets from before v0.21.0: never read, only erased by vault_reset(). */
#define RETIRED_SEC_NVS_NS "sec"

#define LNOTE_BLOB_VERSION 1
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
    char    text[NETDASH_LINK_NOTE_MAX];
} lnote_blob_t;

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

/*
 * Which link ids have a note, and which have a secret. 64 is comfortably
 * above NETDASH_MAX_LINKS.
 */
#define LNOTE_MAX 64
static uint16_t s_lnote_ids[LNOTE_MAX];
static size_t   s_lnote_count;
static uint16_t s_lsec_ids[LNOTE_MAX];
static size_t   s_lsec_count;

/* Defined with the rest of the link code further down. */
static void index_link_notes_locked(void);
static void index_link_secrets_locked(void);

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
    index_link_notes_locked();
    index_link_secrets_locked();
    const size_t lnotes = s_lnote_count;
    const size_t lsecs  = s_lsec_count;
    unlock();

    ESP_LOGI(TAG, "%u link note(s), %u link secret(s), vault %s", (unsigned)lnotes,
             (unsigned)lsecs, vault_configured() ? "configured" : "not set up");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Notes on dashboard links                                                  */
/* ------------------------------------------------------------------------- */

static void lnote_key(uint16_t id, char out[8])
{
    snprintf(out, 8, "%u", (unsigned)id);
}

/* The three below work on either id set. Caller holds the lock. */
static bool ids_have(const uint16_t *set, size_t count, uint16_t id)
{
    for (size_t i = 0; i < count; i++) {
        if (set[i] == id) {
            return true;
        }
    }
    return false;
}

static void ids_add(uint16_t *set, size_t *count, uint16_t id)
{
    if (ids_have(set, *count, id) || *count >= LNOTE_MAX) {
        return;
    }
    set[(*count)++] = id;
}

static void ids_remove(uint16_t *set, size_t *count, uint16_t id)
{
    for (size_t i = 0; i < *count; i++) {
        if (set[i] == id) {
            memmove(&set[i], &set[i + 1], sizeof(set[0]) * (*count - i - 1));
            (*count)--;
            return;
        }
    }
}

/* Fills an id set from every numeric key in a namespace. Caller holds the lock. */
static void index_id_namespace(const char *ns, uint16_t *set, size_t *count)
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

        char      *end = NULL;
        const long id  = strtol(info.key, &end, 10);
        if (end != info.key && end != NULL && *end == 0 && id > 0 && id < 65535) {
            ids_add(set, count, (uint16_t)id);
        }
        fres = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
}

static void index_link_notes_locked(void)
{
    index_id_namespace(LNOTE_NVS_NS, s_lnote_ids, &s_lnote_count);
}

static void index_link_secrets_locked(void)
{
    index_id_namespace(LSEC_NVS_NS, s_lsec_ids, &s_lsec_count);
}

esp_err_t link_note_set(uint16_t link_id, const char *text)
{
    if (link_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[8];
    lnote_key(link_id, key);

    if (text == NULL || text[0] == '\0') {
        esp_err_t err = blob_erase(LNOTE_NVS_NS, key);
        if (err == ESP_OK) {
            lock();
            ids_remove(s_lnote_ids, &s_lnote_count, link_id);
            unlock();
        }
        return err;
    }

    lnote_blob_t blob = {0};
    blob.version      = LNOTE_BLOB_VERSION;
    strncpy(blob.text, text, sizeof(blob.text) - 1);

    esp_err_t err = blob_write(LNOTE_NVS_NS, key, &blob, sizeof(blob));
    if (err == ESP_OK) {
        lock();
        ids_add(s_lnote_ids, &s_lnote_count, link_id);
        unlock();
    } else {
        ESP_LOGW(TAG, "link note not saved for %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

bool link_note_get(uint16_t link_id, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (!link_note_exists(link_id)) {
        return false;
    }

    char key[8];
    lnote_key(link_id, key);

    lnote_blob_t blob;
    if (blob_read(LNOTE_NVS_NS, key, &blob, sizeof(blob)) != ESP_OK ||
        blob.version != LNOTE_BLOB_VERSION) {
        return false;
    }
    blob.text[sizeof(blob.text) - 1] = '\0';
    strncpy(out, blob.text, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

bool link_note_exists(uint16_t link_id)
{
    lock();
    const bool has = ids_have(s_lnote_ids, s_lnote_count, link_id);
    unlock();
    return has;
}

esp_err_t link_note_forget(uint16_t link_id)
{
    return link_note_set(link_id, NULL);
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
 * The owner goes in as additional authenticated data, so a ciphertext lifted
 * out of one link's slot and dropped into another's fails its tag rather than
 * quietly decrypting under the wrong name.
 */
static esp_err_t gcm_encrypt(const uint8_t key[VAULT_KEY_LEN], const uint8_t *aad,
                             size_t aad_len, const uint8_t *iv, const uint8_t *pt,
                             size_t len, uint8_t *ct, uint8_t *tag)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, VAULT_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, iv, VAULT_IV_LEN,
                                       aad, aad_len, pt, ct, VAULT_TAG_LEN, tag);
    }
    mbedtls_gcm_free(&ctx);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t gcm_decrypt(const uint8_t key[VAULT_KEY_LEN], const uint8_t *aad,
                             size_t aad_len, const uint8_t *iv, const uint8_t *tag,
                             const uint8_t *ct, size_t len, uint8_t *pt)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);

    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, VAULT_KEY_LEN * 8);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, len, iv, VAULT_IV_LEN, aad, aad_len, tag,
                                      VAULT_TAG_LEN, ct, pt);
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
/* Secret references                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Everything the vault holds, addressed uniformly: which namespace, which key
 * within it, and the additional authenticated data that binds the ciphertext
 * to its owner. Rotation walks a list of these, so another kind of secret is a
 * matter of extending build_refs() - with AAD shaped unlike "link:<id>", so a
 * ciphertext can never be moved from one kind to the other and authenticate.
 */
typedef struct {
    const char *ns;
    char        key[13];
    uint8_t     aad[16];
    size_t      aad_len;
} sec_ref_t;

static void ref_for_link(sec_ref_t *r, uint16_t id)
{
    r->ns = LSEC_NVS_NS;
    snprintf(r->key, sizeof(r->key), "%u", (unsigned)id);
    r->aad_len = (size_t)snprintf((char *)r->aad, sizeof(r->aad), "link:%u", (unsigned)id);
}

/* Fills out with every stored secret. Returns how many. Takes the lock. */
static size_t build_refs(sec_ref_t *out, size_t cap)
{
    size_t n = 0;

    lock();
    for (size_t i = 0; i < s_lsec_count && n < cap; i++) {
        ref_for_link(&out[n++], s_lsec_ids[i]);
    }
    unlock();
    return n;
}

/* Reads and decrypts one secret into out, which must hold NETDASH_SECRET_MAX. */
static esp_err_t ref_read(const sec_ref_t *r, const uint8_t key[VAULT_KEY_LEN], char *out)
{
    sec_blob_t blob;
    esp_err_t  err = blob_read(r->ns, r->key, &blob, sizeof(blob));
    if (err != ESP_OK) {
        return err;
    }
    if (blob.version != SEC_BLOB_VERSION || blob.len >= NETDASH_SECRET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t len = blob.len;
    err = gcm_decrypt(key, r->aad, r->aad_len, blob.iv, blob.tag, blob.ct, len,
                      (uint8_t *)out);
    if (err == ESP_OK) {
        out[len] = '\0';
    }
    memset(&blob, 0, sizeof(blob));
    return err;
}

/* Encrypts text under key and stores it at r. */
static esp_err_t ref_write(const sec_ref_t *r, const uint8_t key[VAULT_KEY_LEN],
                           const char *text)
{
    const size_t len = strlen(text);
    if (len >= NETDASH_SECRET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    sec_blob_t blob = {0};
    blob.version    = SEC_BLOB_VERSION;
    blob.len        = (uint16_t)len;
    esp_fill_random(blob.iv, sizeof(blob.iv));

    esp_err_t err = gcm_encrypt(key, r->aad, r->aad_len, blob.iv, (const uint8_t *)text,
                                len, blob.ct, blob.tag);
    if (err == ESP_OK) {
        err = blob_write(r->ns, r->key, &blob, sizeof(blob));
    }
    memset(&blob, 0, sizeof(blob));
    return err;
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

    size_t  rotated = 0;
    uint8_t new_key[VAULT_KEY_LEN];
    if (derive_key(new_pass, fresh.salt, fresh.iters, new_key) != ESP_OK) {
        memset(old_key, 0, sizeof(old_key));
        return ESP_FAIL;
    }
    make_verifier(new_key, fresh.verifier);

    /*
     * Re-encrypt everything already stored. Every secret is decrypted into a
     * scratch buffer first, so a single unreadable
     * blob aborts the rotation before anything has been overwritten with a key
     * the rest cannot open.
     */
    esp_err_t err = ESP_OK;

    if (exists) {
        const size_t cap  = LNOTE_MAX;
        sec_ref_t   *refs = calloc(cap, sizeof(*refs));
        if (refs == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            const size_t n = build_refs(refs, cap);

            if (n > 0) {
                char *scratch = calloc(n, NETDASH_SECRET_MAX);
                if (scratch == NULL) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    for (size_t i = 0; i < n && err == ESP_OK; i++) {
                        err = ref_read(&refs[i], old_key, scratch + i * NETDASH_SECRET_MAX);
                    }
                    for (size_t i = 0; i < n && err == ESP_OK; i++) {
                        err = ref_write(&refs[i], new_key, scratch + i * NETDASH_SECRET_MAX);
                    }
                    memset(scratch, 0, n * NETDASH_SECRET_MAX);
                    free(scratch);
                }
            }
            rotated = n;
            free(refs);
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
                 exists ? "changed" : "set", (unsigned)rotated);
    } else {
        ESP_LOGE(TAG, "passphrase change failed: %s", esp_err_to_name(err));
    }

    memset(old_key, 0, sizeof(old_key));
    memset(new_key, 0, sizeof(new_key));
    return err;
}

esp_err_t vault_reset(void)
{
    const size_t cap  = LNOTE_MAX;
    sec_ref_t   *refs = calloc(cap, sizeof(*refs));
    size_t       n    = 0;

    if (refs != NULL) {
        n = build_refs(refs, cap);
        for (size_t i = 0; i < n; i++) {
            blob_erase(refs[i].ns, refs[i].key);
        }
        free(refs);
    }
    esp_err_t err = blob_erase(VAULT_NVS_NS, VAULT_KEY);

    /* Device secrets from older firmware are secrets too. */
    nvs_handle_t h;
    if (nvs_open(RETIRED_SEC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_all(h) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }

    lock();
    s_lsec_count = 0;
    memset(s_lsec_ids, 0, sizeof(s_lsec_ids));
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

static esp_err_t secret_store(const sec_ref_t *r, const char *text, uint16_t *id_set,
                              size_t *id_count, uint16_t id)
{
    if (text == NULL || text[0] == '\0') {
        esp_err_t err = blob_erase(r->ns, r->key);
        if (err == ESP_OK) {
            lock();
            ids_remove(id_set, id_count, id);
            unlock();
        }
        return err;
    }

    if (strlen(text) >= NETDASH_SECRET_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t key[VAULT_KEY_LEN];
    if (!session_key(key)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ref_write(r, key, text);
    memset(key, 0, sizeof(key));

    if (err == ESP_OK) {
        lock();
        ids_add(id_set, id_count, id);
        unlock();
    } else {
        ESP_LOGW(TAG, "secret not saved for %s: %s", r->key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t secret_fetch(const sec_ref_t *r, char *out, size_t cap)
{
    uint8_t key[VAULT_KEY_LEN];
    if (!session_key(key)) {
        return ESP_ERR_INVALID_STATE;
    }

    char      pt[NETDASH_SECRET_MAX];
    esp_err_t err = ref_read(r, key, pt);
    memset(key, 0, sizeof(key));

    if (err == ESP_OK) {
        strncpy(out, pt, cap - 1);
        out[cap - 1] = '\0';
    }
    memset(pt, 0, sizeof(pt));
    return err;
}

/* ------------------------------------------------------------------------- */
/* Secrets on links                                                          */
/* ------------------------------------------------------------------------- */

esp_err_t link_secret_set(uint16_t link_id, const char *text)
{
    if (link_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    sec_ref_t r;
    ref_for_link(&r, link_id);
    return secret_store(&r, text, s_lsec_ids, &s_lsec_count, link_id);
}

esp_err_t link_secret_get(uint16_t link_id, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!link_secret_exists(link_id)) {
        return ESP_ERR_NOT_FOUND;
    }
    sec_ref_t r;
    ref_for_link(&r, link_id);
    return secret_fetch(&r, out, cap);
}

bool link_secret_exists(uint16_t link_id)
{
    lock();
    const bool has = ids_have(s_lsec_ids, s_lsec_count, link_id);
    unlock();
    return has;
}

size_t link_secret_count(void)
{
    lock();
    const size_t n = s_lsec_count;
    unlock();
    return n;
}

esp_err_t link_secret_forget(uint16_t link_id)
{
    /* Deleting needs no key: the ciphertext is simply erased. */
    sec_ref_t r;
    ref_for_link(&r, link_id);

    esp_err_t err = blob_erase(r.ns, r.key);
    if (err == ESP_OK) {
        lock();
        ids_remove(s_lsec_ids, &s_lsec_count, link_id);
        unlock();
    }
    return err;
}
