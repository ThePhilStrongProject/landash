/*
 * Over-the-air updates from GitHub releases. See ota.h for the design and the
 * reasons behind it; this file is the mechanics.
 */
#include "ota.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "notify.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "ota";

#define OTA_USER_AGENT      "LANDA.SH-dongle"
#define FIRST_CHECK_DELAY_S 120     /* let discovery have the first minutes  */
#define RETRY_OFFLINE_S     300     /* no Wi-Fi yet: try again this soon     */
#define CONFIRM_AFTER_S     60      /* a new image must survive this long... */
#define CONFIRM_DEADLINE_S  600     /* ...and be online by this, or roll back */
#define MANIFEST_MAX        1024    /* latest.json is ~100 bytes             */

#define REQ_CHECK   (1u << 0)
#define REQ_INSTALL (1u << 1)

#define OTA_NVS_NS   "ota"
#define NVS_KEY_FROM "from"         /* version we updated from, for the feed */
#define NVS_KEY_RBK  "rbk"          /* rolled-back version already announced */

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static ota_status_t      s_status;

/* Only the task touches these. */
static char     s_file_url[256];    /* the image latest.json names          */
static uint32_t s_file_size;        /* its size, 0 when the manifest omits it */
static bool     s_pending_verify;   /* running image not yet confirmed good  */

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void set_state(ota_state_t st, const char *error)
{
    lock();
    s_status.state = st;
    if (error != NULL) {
        snprintf(s_status.error, sizeof(s_status.error), "%s", error);
    } else if (st != OTA_STATE_ERROR) {
        s_status.error[0] = '\0';
    }
    unlock();
}

static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t : 0;   /* 0 until NTP has synced */
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

const char *ota_state_name(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_IDLE:        return "idle";
    case OTA_STATE_CHECKING:    return "checking";
    case OTA_STATE_UP_TO_DATE:  return "up_to_date";
    case OTA_STATE_AVAILABLE:   return "available";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_REBOOTING:   return "rebooting";
    case OTA_STATE_ERROR:       return "error";
    default:                    return "unknown";
    }
}

const char *ota_repo(void)
{
    return CONFIG_NETDASH_OTA_REPO;
}

/* ------------------------------------------------------------------------- */
/* Versions                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * "v1.2.3" (the v optional) into v[]. *rest gets whatever follows the patch
 * number: "" for a build made exactly on a tag, "-3-gabc1234" for one made
 * three commits later, "-dirty" for uncommitted changes.
 */
static bool parse_version(const char *s, int v[3], const char **rest)
{
    if (s == NULL) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        char *end;
        v[i] = (int)strtol(s, &end, 10);
        s    = end;
        if (i < 2) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    if (rest != NULL) {
        *rest = s;
    }
    return true;
}

/*
 * True when tag is a newer release than the running version. A build a few
 * commits past v0.12.0 is not behind v0.12.0, so only the numbers compare; an
 * unparseable running version (a bare commit hash) counts as behind anything.
 */
static bool is_newer(const char *tag, const char *current)
{
    int t[3], c[3];
    if (!parse_version(tag, t, NULL)) {
        return false;
    }
    if (!parse_version(current, c, NULL)) {
        return true;
    }
    for (int i = 0; i < 3; i++) {
        if (t[i] != c[i]) {
            return t[i] > c[i];
        }
    }
    return false;
}

/*
 * A development build - uncommitted changes, or a bare commit hash with no tag
 * behind it - is never replaced automatically: whoever flashed it by hand is
 * working on it. A clean build a few commits past a tag is fair game, since
 * any newer release is newer than what it was built from.
 */
static bool is_release_build(const char *current)
{
    int         c[3];
    const char *rest = NULL;
    return parse_version(current, c, &rest) && strstr(current, "dirty") == NULL;
}

/* ------------------------------------------------------------------------- */
/* The manifest                                                              */
/* ------------------------------------------------------------------------- */

/*
 * The releases repository holds latest.json next to the images it names:
 *
 *     {"version": "v0.14.1", "file": "firmware/netdash-v0.14.1.bin", "size": 1982176}
 *
 * Both come from raw.githubusercontent.com, straight off the branch, so
 * publishing is a commit and a push: no release objects, no API, and no
 * redirect to a signed URL on some other host. The manifest may say which
 * image, never where from - "file" must be a plain path inside the repository.
 */

static void raw_url(char *out, size_t cap, const char *path)
{
    snprintf(out, cap, "https://raw.githubusercontent.com/%s/%s/%s", ota_repo(),
             CONFIG_NETDASH_OTA_BRANCH, path);
}

static bool safe_repo_path(const char *p)
{
    if (p == NULL || p[0] == '\0' || p[0] == '/' || strlen(p) > 96 || strstr(p, "..") != NULL) {
        return false;
    }
    for (const char *c = p; *c != '\0'; c++) {
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-' && *c != '/') {
            return false;
        }
    }
    return true;
}

static const char *status_error(int code)
{
    switch (code) {
    case 401:
    case 403: return "GitHub refused the request (is the releases repository public?)";
    case 404: return "No latest.json found in the releases repository";
    case 429: return "GitHub is rate-limiting requests; it will try again later";
    default:  return "Unexpected answer from GitHub";
    }
}

/* Fills version, s_file_url and s_file_size from latest.json. */
static esp_err_t fetch_manifest(char *version, size_t vcap, char *err, size_t err_cap)
{
    char url[160];
    raw_url(url, sizeof(url), "latest.json");

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 1024,
        .buffer_size_tx    = 1024,
        .user_agent        = OTA_USER_AGENT,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        snprintf(err, err_cap, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        snprintf(err, err_cap, "Could not reach GitHub (%s)", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return e;
    }
    (void)esp_http_client_fetch_headers(c);
    const int code = esp_http_client_get_status_code(c);
    if (code != 200) {
        snprintf(err, err_cap, "%s (HTTP %d)", status_error(code), code);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    char *buf = malloc(MANIFEST_MAX);
    int   len = 0;
    int   n   = 0;
    if (buf != NULL) {
        while (len < MANIFEST_MAX - 1 &&
               (n = esp_http_client_read(c, buf + len, MANIFEST_MAX - 1 - len)) > 0) {
            len += n;
        }
    }
    const bool more = buf != NULL && len == MANIFEST_MAX - 1 && esp_http_client_read(c, url, 1) > 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (buf == NULL) {
        snprintf(err, err_cap, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (n < 0 || more) {
        free(buf);
        snprintf(err, err_cap, more ? "latest.json is too large" : "Connection dropped reading latest.json");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    const cJSON *jv = cJSON_GetObjectItemCaseSensitive(j, "version");
    const cJSON *jf = cJSON_GetObjectItemCaseSensitive(j, "file");
    const cJSON *js = cJSON_GetObjectItemCaseSensitive(j, "size");
    int          ver[3];
    e = ESP_OK;
    if (j == NULL) {
        snprintf(err, err_cap, "latest.json is not valid JSON");
        e = ESP_FAIL;
    } else if (!cJSON_IsString(jv) || strlen(jv->valuestring) >= vcap ||
               !parse_version(jv->valuestring, ver, NULL)) {
        snprintf(err, err_cap, "latest.json has no usable \"version\"");
        e = ESP_FAIL;
    } else if (!cJSON_IsString(jf) || !safe_repo_path(jf->valuestring)) {
        snprintf(err, err_cap, "latest.json has no usable \"file\"");
        e = ESP_FAIL;
    } else {
        snprintf(version, vcap, "%s", jv->valuestring);
        raw_url(s_file_url, sizeof(s_file_url), jf->valuestring);
        s_file_size = cJSON_IsNumber(js) && js->valuedouble > 0 ? (uint32_t)js->valuedouble : 0;
    }
    cJSON_Delete(j);
    return e;
}

/* ------------------------------------------------------------------------- */
/* Check and install                                                         */
/* ------------------------------------------------------------------------- */

static bool sta_online(void)
{
    const wifi_mgr_mode_t mode = wifi_mgr_get_mode();
    if (mode != WIFI_MGR_MODE_STA && mode != WIFI_MGR_MODE_APSTA) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return wifi_mgr_get_ip_info(&ip) == ESP_OK && ip.ip.addr != 0;
}

static void nvs_put_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (val != NULL) {
        (void)nvs_set_str(h, key, val);
    } else {
        (void)nvs_erase_key(h, key);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

static bool nvs_get_str_buf(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    const bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* Returns true when a newer release is available and s_file_url is set. */
static bool do_check(void)
{
        if (ota_repo()[0] == '\0') {
        set_state(OTA_STATE_ERROR, "No update repository is built into this firmware");
        return false;
    }
    if (!sta_online()) {
        set_state(OTA_STATE_ERROR, "Not connected to Wi-Fi");
        return false;
    }

    set_state(OTA_STATE_CHECKING, NULL);
    char tag[32] = "";
    char err[80] = "";
    const esp_err_t e = fetch_manifest(tag, sizeof(tag), err, sizeof(err));

    lock();
    s_status.last_check = now_unix();
    if (tag[0] != '\0') {
        snprintf(s_status.latest, sizeof(s_status.latest), "%s", tag);
    }
    const bool newer = tag[0] != '\0' && is_newer(tag, s_status.current);
    s_status.auto_blocked = newer && (!is_release_build(s_status.current) ||
                                      strcmp(tag, s_status.rolled_back) == 0);
    unlock();

    if (e != ESP_OK) {
        ESP_LOGW(TAG, "check failed: %s", err);
        set_state(OTA_STATE_ERROR, err);
        return false;
    }
    if (!newer) {
        ESP_LOGI(TAG, "up to date (running %s, latest %s)", s_status.current, tag);
        set_state(OTA_STATE_UP_TO_DATE, NULL);
        return false;
    }
    ESP_LOGI(TAG, "%s is available (running %s)", tag, s_status.current);
    set_state(OTA_STATE_AVAILABLE, NULL);
    return true;
}

/*
 * The download resumes. The first real update died about 30 s into 2 MB: the
 * server closed the connection cleanly partway through, and esp_https_ota
 * cannot carry on from a byte offset, so the whole attempt was thrown away.
 * Writing with esp_ota_* directly lets a stream that stops short be reopened
 * with a Range request at the byte it had reached.
 */
#define DL_CHUNK          4096
#define DL_ATTEMPTS       6
#define DL_RETRY_DELAY_MS 3000
#define DL_LOG_EVERY      (256 * 1024)

/* Opens the image at offset. Returns NULL when it could not connect at all. */
static esp_http_client_handle_t open_image_at(uint32_t offset, int *code, int64_t *length)
{
    esp_http_client_config_t cfg = {
        .url               = s_file_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 1024,
        .user_agent        = OTA_USER_AGENT,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return NULL;
    }
    if (offset > 0) {
        char range[32];
        snprintf(range, sizeof(range), "bytes=%" PRIu32 "-", offset);
        esp_http_client_set_header(c, "Range", range);
    }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    *length = esp_http_client_fetch_headers(c);
    *code   = esp_http_client_get_status_code(c);
    return c;
}

static void close_image(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

/*
 * The image has to name itself as this project and as the version latest.json
 * promised. That catches a manifest pointing at the wrong file, or a build
 * made from uncommitted changes, before a byte of it is written.
 */
static bool image_header_ok(const uint8_t *p, size_t n, const char *tag, char *err, size_t err_cap)
{
    const size_t at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < at + sizeof(esp_app_desc_t) || p[0] != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(err, err_cap, "The file is not a firmware image");
        return false;
    }
    esp_app_desc_t desc;
    memcpy(&desc, p + at, sizeof(desc));
    const esp_app_desc_t *running = esp_app_get_description();
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        snprintf(err, err_cap, "The file is not a %s image", running->project_name);
        return false;
    }
    if (strncmp(desc.version, tag, sizeof(desc.version)) != 0) {
        snprintf(err, err_cap, "The image says %.20s, latest.json says %.20s", desc.version, tag);
        return false;
    }
    return true;
}

/* Downloads and installs the release found by do_check(). Reboots on success. */
static void do_install(void)
{
    static uint8_t buf[DL_CHUNK];   /* task-owned, off the stack */

    char tag[32];
    lock();
    snprintf(tag, sizeof(tag), "%s", s_status.latest);
    s_status.bytes_done  = 0;
    s_status.bytes_total = s_file_size;
    s_status.state       = OTA_STATE_DOWNLOADING;
    s_status.error[0]    = '\0';
    unlock();

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        set_state(OTA_STATE_ERROR, "No partition to install into");
        return;
    }
    uint32_t total = s_file_size;
    if (total > part->size) {
        set_state(OTA_STATE_ERROR, "The image is larger than the app partition");
        return;
    }

    ESP_LOGI(TAG, "downloading %s (%" PRIu32 " bytes) from %s", tag, total, s_file_url);

    char             err[80] = "";
    esp_ota_handle_t ota     = 0;
    bool             begun   = false;   /* esp_ota_begin() done              */
    bool             fatal   = false;   /* retrying cannot help              */
    bool             done_ok = false;
    uint32_t         done    = 0;
    uint32_t         next_log = DL_LOG_EVERY;

    for (int attempt = 1; attempt <= DL_ATTEMPTS && !done_ok && !fatal; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "%s; resuming at %" PRIu32 " of %" PRIu32 " (attempt %d, heap %u, largest %u)",
                     err, done, total, attempt, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            vTaskDelay(pdMS_TO_TICKS(DL_RETRY_DELAY_MS));
        }

        int                      code   = 0;
        int64_t                  length = 0;
        esp_http_client_handle_t c      = open_image_at(done, &code, &length);
        if (c == NULL) {
            snprintf(err, sizeof(err), "Could not reach GitHub");
            continue;
        }
        if (code != 200 && !(done > 0 && code == 206)) {
            snprintf(err, sizeof(err), "%s (HTTP %d)", status_error(code), code);
            close_image(c);
            fatal = code == 401 || code == 403 || code == 404;
            continue;
        }
        if (total == 0 && code == 200 && length > 0) {
            total = (uint32_t)length;
            lock();
            s_status.bytes_total = total;
            unlock();
        }
        /* A server that ignored the Range header starts from zero again. */
        uint32_t skip = (code == 200) ? done : 0;

        int n;
        while (!fatal && (n = esp_http_client_read(c, (char *)buf, sizeof(buf))) > 0) {
            const uint8_t *p = buf;
            size_t         m = (size_t)n;
            if (skip > 0) {
                const size_t s = skip < m ? skip : m;
                p += s;
                m -= s;
                skip -= (uint32_t)s;
                if (m == 0) {
                    continue;
                }
            }
            if (!begun) {
                if (!image_header_ok(p, m, tag, err, sizeof(err))) {
                    fatal = true;
                    break;
                }
                esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
                if (e != ESP_OK) {
                    snprintf(err, sizeof(err), "Could not start writing (%s)", esp_err_to_name(e));
                    fatal = true;
                    break;
                }
                begun = true;
            }
            esp_err_t e = esp_ota_write(ota, p, m);
            if (e != ESP_OK) {
                snprintf(err, sizeof(err), "Writing the image failed (%s)", esp_err_to_name(e));
                fatal = true;
                break;
            }
            done += (uint32_t)m;
            lock();
            s_status.bytes_done = done;
            unlock();
            if (done >= next_log) {
                ESP_LOGI(TAG, "%" PRIu32 " of %" PRIu32 " bytes (heap %u)", done, total,
                         (unsigned)esp_get_free_heap_size());
                next_log += DL_LOG_EVERY;
            }
        }
        close_image(c);

        if (fatal) {
            break;
        }
        if (total > 0 && done >= total) {
            done_ok = true;
        } else if (total == 0 && n == 0 && begun) {
            done_ok = true;   /* no length to check against; the image hash will */
        } else {
            snprintf(err, sizeof(err), "Download stopped at %" PRIu32 " of %" PRIu32 " bytes",
                     done, total);
        }
    }

    if (!done_ok) {
        if (begun) {
            esp_ota_abort(ota);
        }
        ESP_LOGW(TAG, "%s", err);
        set_state(OTA_STATE_ERROR, err);
        return;
    }

    /* esp_ota_end() checks the image's own SHA-256 and structure. */
    esp_err_t e = esp_ota_end(ota);
    if (e == ESP_OK) {
        e = esp_ota_set_boot_partition(part);
    }
    if (e != ESP_OK) {
        snprintf(err, sizeof(err), "The image did not verify (%s)", esp_err_to_name(e));
        ESP_LOGW(TAG, "%s", err);
        set_state(OTA_STATE_ERROR, err);
        return;
    }

    ESP_LOGI(TAG, "installed %s, restarting", tag);
    nvs_put_str(NVS_KEY_FROM, s_status.current);   /* for the feed, after reboot */
    set_state(OTA_STATE_REBOOTING, NULL);
    vTaskDelay(pdMS_TO_TICKS(2000));   /* let a polling page see "rebooting" */
    esp_restart();
}

/* ------------------------------------------------------------------------- */
/* Rollback bookkeeping                                                      */
/* ------------------------------------------------------------------------- */

/* Called at boot: learn whether this image is on probation, or replaced one. */
static void inspect_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   st;
    if (running != NULL && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGI(TAG, "new image on probation until it has been up and online");
    }

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t         desc;
    if (bad != NULL && esp_ota_get_partition_description(bad, &desc) == ESP_OK) {
        snprintf(s_status.rolled_back, sizeof(s_status.rolled_back), "%s", desc.version);
        ESP_LOGW(TAG, "%s was rolled back; it will not be installed automatically again",
                 desc.version);
    }
}

/*
 * Once per boot at most: marks a probationary image good, or rolls it back,
 * and tells the feed what happened. The feed is only armed after the first
 * sweep, so the announcements wait for that.
 */
static void housekeeping(void)
{
    static bool announced;

    if (s_pending_verify) {
        const uint32_t up = uptime_s();
        const bool online = !settings_wifi_configured() || sta_online();
        if (up >= CONFIRM_AFTER_S && online) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_pending_verify = false;
            ESP_LOGI(TAG, "new image confirmed good");
        } else if (up >= CONFIRM_DEADLINE_S) {
            ESP_LOGE(TAG, "new image never got online; rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }

    if (announced || s_pending_verify || !notify_armed()) {
        return;
    }
    announced = true;

    char text[NETDASH_NOTIF_TEXT];
    char from[32];
    if (nvs_get_str_buf(NVS_KEY_FROM, from, sizeof(from))) {
        snprintf(text, sizeof(text), "Updated to %.18s (was %.18s)", s_status.current, from);
        notify_push(NETDASH_NOTIF_UPDATE, NULL, 0, text);
        nvs_put_str(NVS_KEY_FROM, NULL);
    }
    char seen[32];
    if (s_status.rolled_back[0] != '\0' &&
        (!nvs_get_str_buf(NVS_KEY_RBK, seen, sizeof(seen)) ||
         strcmp(seen, s_status.rolled_back) != 0)) {
        snprintf(text, sizeof(text), "%.20s failed to start; rolled back",
                 s_status.rolled_back);
        notify_push(NETDASH_NOTIF_UPDATE, NULL, 0, text);
        nvs_put_str(NVS_KEY_RBK, s_status.rolled_back);
    }
}

/* ------------------------------------------------------------------------- */
/* Task                                                                      */
/* ------------------------------------------------------------------------- */

static void ota_task(void *arg)
{
    (void)arg;
    int64_t next_check_us = esp_timer_get_time() + (int64_t)FIRST_CHECK_DELAY_S * 1000000;
    char    announced_tag[32] = "";

    for (;;) {
        netdash_settings_t cfg;
        settings_get(&cfg);

        /* Short naps while there is probation to watch; a minute otherwise, so
           a change to the settings is picked up without a restart. */
        uint32_t wait_ms = s_pending_verify ? 5000 : 60000;
        if (cfg.ota_enabled) {
            const int64_t until = next_check_us - esp_timer_get_time();
            if (until <= 0) {
                wait_ms = 0;
            } else if (until / 1000 < wait_ms) {
                wait_ms = (uint32_t)(until / 1000);
            }
        }

        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(wait_ms));
        housekeeping();

        const bool due = cfg.ota_enabled && esp_timer_get_time() >= next_check_us;
        if ((bits & (REQ_CHECK | REQ_INSTALL)) == 0 && !due) {
            continue;
        }

        const bool available = do_check();
        const bool reachable = s_status.state != OTA_STATE_ERROR ||
                               strcmp(s_status.error, "Not connected to Wi-Fi") != 0;
        next_check_us = esp_timer_get_time() +
                        (int64_t)(reachable ? cfg.ota_interval_h * 3600 : RETRY_OFFLINE_S) * 1000000;
        if (!available) {
            continue;
        }

        if ((bits & REQ_INSTALL) != 0) {
            do_install();   /* a person asked: no gate but the checks inside */
        } else if (cfg.ota_auto && !s_status.auto_blocked) {
            do_install();
        } else if (strcmp(announced_tag, s_status.latest) != 0) {
            /* Not installing it ourselves, so say it is there - once. */
            char text[NETDASH_NOTIF_TEXT];
            snprintf(text, sizeof(text), "%.24s is available", s_status.latest);
            notify_push(NETDASH_NOTIF_UPDATE, NULL, 0, text);
            snprintf(announced_tag, sizeof(announced_tag), "%s", s_status.latest);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t ota_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.current, sizeof(s_status.current), "%s", esp_app_get_description()->version);
    s_status.state = OTA_STATE_IDLE;
    inspect_boot();

    /* TLS handshakes are stack-hungry; 8 KB is what Espressif's OTA examples use. */
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "updates from github.com/%s (%s/latest.json)", ota_repo(),
             CONFIG_NETDASH_OTA_BRANCH);
    return ESP_OK;
}

void ota_get_status(ota_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_status;
    unlock();
}

esp_err_t ota_check_now(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_task, REQ_CHECK, eSetBits);
    return ESP_OK;
}

esp_err_t ota_install_now(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const bool busy = s_status.state == OTA_STATE_DOWNLOADING ||
                      s_status.state == OTA_STATE_REBOOTING;
    unlock();
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_task, REQ_INSTALL, eSetBits);
    return ESP_OK;
}
