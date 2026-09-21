/*
 * Over-the-air updates from GitHub releases. See ota.h for the design and the
 * reasons behind it; this file is the mechanics.
 */
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
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
#define OTA_API_VERSION     "2022-11-28"
#define FIRST_CHECK_DELAY_S 120     /* let discovery have the first minutes  */
#define RETRY_OFFLINE_S     300     /* no Wi-Fi yet: try again this soon     */
#define CONFIRM_AFTER_S     60      /* a new image must survive this long... */
#define CONFIRM_DEADLINE_S  600     /* ...and be online by this, or roll back */
#define URL_MAX             192     /* api.github.com asset URLs are ~80     */
#define REDIRECT_URL_MAX    2048    /* the signed download URL is ~800       */

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
static char     s_asset_url[URL_MAX];
static uint32_t s_asset_size;
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

/* Bounded copy that says so: truncation here is intended, not a bug. */
static void copy_str(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
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
/* Streaming scan of the GitHub release JSON                                 */
/* ------------------------------------------------------------------------- */

/*
 * GET /repos/{repo}/releases/latest answers with several kilobytes - release
 * notes, uploader profiles, a block per asset - of which we need three
 * values: tag_name, and the url and size of the asset with our name. Parsing
 * it with cJSON would need the whole text and its tree in RAM next to a live
 * TLS session, so this tokenizer walks it a chunk at a time instead, keeping
 * only the key path and the few strings that matter.
 */

#define JS_MAX_DEPTH 8

typedef struct {
    uint8_t  depth;                         /* containers open               */
    uint8_t  overflow;                      /* extra depth beyond MAX_DEPTH  */
    char     kind[JS_MAX_DEPTH];            /* '{' or '['                    */
    bool     want_key[JS_MAX_DEPTH];        /* object: next string is a key  */
    char     key[JS_MAX_DEPTH][20];         /* last key seen at each level   */

    bool     in_str, esc, str_is_key, keep;
    uint8_t  uskip;                         /* \uXXXX hex digits to skip     */
    char     str[URL_MAX];
    size_t   slen;
    char     lit[16];
    size_t   llen;

    /* The asset object being read, then the one that matched. */
    char     a_url[URL_MAX];
    char     a_name[64];
    uint32_t a_size;

    char     tag[32];
    char     url[URL_MAX];
    uint32_t size;
    bool     found;
} gh_scan_t;

static bool in_asset_object(const gh_scan_t *s)
{
    return s->overflow == 0 && s->depth == 3 && s->kind[0] == '{' &&
           strcmp(s->key[0], "assets") == 0 && s->kind[1] == '[' && s->kind[2] == '{';
}

static bool at_root_key(const gh_scan_t *s, const char *k)
{
    return s->overflow == 0 && s->depth == 1 && s->kind[0] == '{' && strcmp(s->key[0], k) == 0;
}

static void scan_end_literal(gh_scan_t *s)
{
    if (s->llen == 0) {
        return;
    }
    s->lit[s->llen] = '\0';
    if (in_asset_object(s) && strcmp(s->key[2], "size") == 0) {
        s->a_size = (uint32_t)strtoul(s->lit, NULL, 10);
    }
    s->llen = 0;
}

static void scan_end_string(gh_scan_t *s)
{
    s->str[s->slen] = '\0';
    if (s->str_is_key) {
        if (s->overflow == 0 && s->depth > 0) {
            copy_str(s->key[s->depth - 1], sizeof(s->key[0]), s->str);
            s->want_key[s->depth - 1] = false;
        }
        return;
    }
    if (!s->keep) {
        return;
    }
    if (at_root_key(s, "tag_name")) {
        copy_str(s->tag, sizeof(s->tag), s->str);
    } else if (in_asset_object(s) && strcmp(s->key[2], "url") == 0) {
        copy_str(s->a_url, sizeof(s->a_url), s->str);
    } else if (in_asset_object(s) && strcmp(s->key[2], "name") == 0) {
        copy_str(s->a_name, sizeof(s->a_name), s->str);
    }
}

static void scan_open(gh_scan_t *s, char c)
{
    scan_end_literal(s);
    if (s->overflow > 0 || s->depth == JS_MAX_DEPTH) {
        s->overflow++;
        return;
    }
    s->kind[s->depth]     = c;
    s->want_key[s->depth] = (c == '{');
    s->key[s->depth][0]   = '\0';
    s->depth++;
    if (in_asset_object(s)) {
        s->a_url[0]  = '\0';
        s->a_name[0] = '\0';
        s->a_size    = 0;
    }
}

static void scan_close(gh_scan_t *s, const char *asset_name)
{
    scan_end_literal(s);
    if (s->overflow > 0) {
        s->overflow--;
        return;
    }
    if (in_asset_object(s) && !s->found && strcmp(s->a_name, asset_name) == 0 &&
        s->a_url[0] != '\0') {
        snprintf(s->url, sizeof(s->url), "%s", s->a_url);
        s->size  = s->a_size;
        s->found = true;
    }
    if (s->depth > 0) {
        s->depth--;
    }
}

static void scan_feed(gh_scan_t *s, const char *buf, size_t n, const char *asset_name)
{
    for (size_t i = 0; i < n; i++) {
        const char c = buf[i];

        if (s->in_str) {
            if (s->uskip > 0) {
                s->uskip--;
                continue;
            }
            if (s->esc) {
                s->esc = false;
                char out = c;
                if (c == 'u') {
                    s->uskip = 4;
                    out      = '?';   /* nothing we keep is outside ASCII */
                }
                if (s->keep && s->slen < sizeof(s->str) - 1) {
                    s->str[s->slen++] = out;
                }
                continue;
            }
            if (c == '\\') {
                s->esc = true;
            } else if (c == '"') {
                s->in_str = false;
                scan_end_string(s);
            } else if (s->keep && s->slen < sizeof(s->str) - 1) {
                s->str[s->slen++] = c;
            }
            continue;
        }

        switch (c) {
        case '"': {
            scan_end_literal(s);
            const bool is_key = s->overflow == 0 && s->depth > 0 &&
                                s->kind[s->depth - 1] == '{' && s->want_key[s->depth - 1];
            s->in_str     = true;
            s->str_is_key = is_key;
            s->slen       = 0;
            s->keep       = is_key || at_root_key(s, "tag_name") ||
                            (in_asset_object(s) && (strcmp(s->key[2], "url") == 0 ||
                                                    strcmp(s->key[2], "name") == 0));
            break;
        }
        case '{':
        case '[':
            scan_open(s, c);
            break;
        case '}':
        case ']':
            scan_close(s, asset_name);
            break;
        case ',':
            scan_end_literal(s);
            if (s->overflow == 0 && s->depth > 0 && s->kind[s->depth - 1] == '{') {
                s->want_key[s->depth - 1] = true;
            }
            break;
        case ':':
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            scan_end_literal(s);
            break;
        default:
            if (s->llen < sizeof(s->lit) - 1) {
                s->lit[s->llen++] = c;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* GitHub                                                                    */
/* ------------------------------------------------------------------------- */

static void add_github_headers(esp_http_client_handle_t c, const char *accept, const char *token)
{
    esp_http_client_set_header(c, "Accept", accept);
    esp_http_client_set_header(c, "X-GitHub-Api-Version", OTA_API_VERSION);
    if (token != NULL && token[0] != '\0') {
        char auth[sizeof(((netdash_settings_t *)0)->ota_token) + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(c, "Authorization", auth);
    }
}

static const char *status_error(int code, bool have_token)
{
    switch (code) {
    case 401: return "GitHub rejected the access token";
    case 403: return "GitHub refused the request (rate limit or token permissions)";
    case 404: return have_token ? "No release found, or the token cannot see the repository"
                                : "No release found (a private repository needs a token)";
    default:  return "Unexpected answer from GitHub";
    }
}

/* Fills tag / s_asset_url / s_asset_size from the latest release. */
static esp_err_t fetch_latest(const char *token, char *tag, size_t tag_cap, char *err, size_t err_cap)
{
    static gh_scan_t scan;   /* ~700 bytes, task-owned, off the stack */
    static char      buf[512];

    char url[128];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", ota_repo());

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
    add_github_headers(c, "application/vnd.github+json", token);

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        snprintf(err, err_cap, "Could not reach api.github.com (%s)", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return e;
    }
    (void)esp_http_client_fetch_headers(c);
    const int code = esp_http_client_get_status_code(c);
    if (code != 200) {
        snprintf(err, err_cap, "%s (HTTP %d)", status_error(code, token[0] != '\0'), code);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    memset(&scan, 0, sizeof(scan));
    int n;
    while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        scan_feed(&scan, buf, (size_t)n, CONFIG_NETDASH_OTA_ASSET);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (n < 0) {
        snprintf(err, err_cap, "Connection dropped while reading the release");
        return ESP_FAIL;
    }
    if (scan.tag[0] == '\0') {
        snprintf(err, err_cap, "The release has no tag");
        return ESP_FAIL;
    }
    snprintf(tag, tag_cap, "%s", scan.tag);
    if (!scan.found) {
        snprintf(err, err_cap, "Release %s has no %s attached", scan.tag, CONFIG_NETDASH_OTA_ASSET);
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(s_asset_url, sizeof(s_asset_url), "%s", scan.url);
    s_asset_size = scan.size;
    return ESP_OK;
}

/*
 * The asset URL on api.github.com answers with a redirect to a short-lived
 * signed URL on another host. That host must NOT see the token - it rejects
 * requests carrying one, and there is no reason to hand it over anyway - so
 * the redirect is resolved here by hand and the download made without it.
 *
 * The Location header is taken straight from the response. Do not be tempted
 * by esp_http_client_set_redirection() + esp_http_client_get_url(): get_url
 * rebuilds the URL from scheme, host and path and drops the query string,
 * which is where the signature lives, and the download host then answers
 * with nonsense ("Server error (618)").
 */
typedef struct {
    char  *buf;
    size_t cap;
    bool   got;
    bool   too_long;
} location_ctx_t;

static esp_err_t location_event(esp_http_client_event_t *evt)
{
    location_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx != NULL && evt->header_key != NULL &&
        evt->header_value != NULL && strcasecmp(evt->header_key, "Location") == 0) {
        if (strlen(evt->header_value) >= ctx->cap) {
            ctx->too_long = true;
        } else {
            strcpy(ctx->buf, evt->header_value);
            ctx->got = true;
        }
    }
    return ESP_OK;
}

static esp_err_t resolve_download(const char *token, char *out, size_t cap, char *err, size_t err_cap)
{
    location_ctx_t loc = {.buf = out, .cap = cap};
    esp_http_client_config_t cfg = {
        .url                   = s_asset_url,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 15000,
        .buffer_size           = 2048,
        .buffer_size_tx        = 1024,
        .user_agent            = OTA_USER_AGENT,
        .disable_auto_redirect = true,
        .event_handler         = location_event,
        .user_data             = &loc,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        snprintf(err, err_cap, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    add_github_headers(c, "application/octet-stream", token);

    esp_err_t e = esp_http_client_open(c, 0);
    if (e == ESP_OK) {
        (void)esp_http_client_fetch_headers(c);
        const int code = esp_http_client_get_status_code(c);
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            if (!loc.got) {
                snprintf(err, err_cap, loc.too_long ? "The download redirect is too long"
                                                    : "The download redirect had no Location");
                e = ESP_FAIL;
            }
        } else {
            snprintf(err, err_cap, "%s (HTTP %d)", status_error(code, token[0] != '\0'), code);
            e = ESP_FAIL;
        }
        esp_http_client_close(c);
    } else {
        snprintf(err, err_cap, "Could not reach api.github.com (%s)", esp_err_to_name(e));
    }
    esp_http_client_cleanup(c);
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

/* Returns true when a newer release is available and s_asset_url is set. */
static bool do_check(void)
{
    netdash_settings_t cfg;
    settings_get(&cfg);

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
    const esp_err_t e = fetch_latest(cfg.ota_token, tag, sizeof(tag), err, sizeof(err));

    lock();
    s_status.last_check = now_unix();
    if (tag[0] != '\0') {
        snprintf(s_status.latest, sizeof(s_status.latest), "%s", tag);
    }
    const bool newer = tag[0] != '\0' && is_newer(tag, s_status.current);
    s_status.auto_blocked = newer && (!is_release_build(s_status.current) ||
                                      strcmp(tag, s_status.rolled_back) == 0);
    unlock();

    if (e == ESP_ERR_NOT_FOUND && !newer) {
        set_state(OTA_STATE_UP_TO_DATE, NULL);   /* nothing to install anyway */
        return false;
    }
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

/* Downloads and installs the release found by do_check(). Reboots on success. */
static void do_install(void)
{
    netdash_settings_t cfg;
    settings_get(&cfg);

    char tag[32];
    lock();
    snprintf(tag, sizeof(tag), "%s", s_status.latest);
    s_status.bytes_done  = 0;
    s_status.bytes_total = s_asset_size;
    s_status.state       = OTA_STATE_DOWNLOADING;
    s_status.error[0]    = '\0';
    unlock();

    char  err[80] = "";
    char *url     = malloc(REDIRECT_URL_MAX);
    if (url == NULL) {
        set_state(OTA_STATE_ERROR, "Out of memory");
        return;
    }
    if (resolve_download(cfg.ota_token, url, REDIRECT_URL_MAX, err, sizeof(err)) != ESP_OK) {
        free(url);
        set_state(OTA_STATE_ERROR, err);
        return;
    }

    /* Just the host: the query is a signature, and too long to be worth logging. */
    const char *host = strstr(url, "://");
    host             = host != NULL ? host + 3 : url;
    ESP_LOGI(TAG, "download redirected to %.*s", (int)strcspn(host, "/?"), host);

    esp_http_client_config_t http = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,   /* the request line carries the long URL */
        .user_agent        = OTA_USER_AGENT,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http,
    };

    ESP_LOGI(TAG, "downloading %s", tag);
    esp_https_ota_handle_t h = NULL;
    esp_err_t              e = esp_https_ota_begin(&ota_cfg, &h);
    free(url);   /* esp_http_client took its own copy */
    if (e != ESP_OK) {
        snprintf(err, sizeof(err), "Download failed to start (%s)", esp_err_to_name(e));
        set_state(OTA_STATE_ERROR, err);
        return;
    }

    /*
     * The image has to name itself as this project and as the release it was
     * attached to. That catches a binary uploaded to the wrong release, or a
     * build made from uncommitted changes, before it is written anywhere.
     */
    esp_app_desc_t desc;
    e = esp_https_ota_get_img_desc(h, &desc);
    const esp_app_desc_t *running = esp_app_get_description();
    if (e != ESP_OK) {
        snprintf(err, sizeof(err), "Could not read the image header");
    } else if (strncmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        snprintf(err, sizeof(err), "The asset is not a %s image", running->project_name);
        e = ESP_ERR_INVALID_VERSION;
    } else if (strncmp(desc.version, tag, sizeof(desc.version)) != 0) {
        snprintf(err, sizeof(err), "The asset says %.20s, the release says %.20s", desc.version, tag);
        e = ESP_ERR_INVALID_VERSION;
    }
    if (e != ESP_OK) {
        esp_https_ota_abort(h);
        ESP_LOGW(TAG, "%s", err);
        set_state(OTA_STATE_ERROR, err);
        return;
    }

    const int total = esp_https_ota_get_image_size(h);
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        lock();
        s_status.bytes_done = (uint32_t)esp_https_ota_get_image_len_read(h);
        if (total > 0) {
            s_status.bytes_total = (uint32_t)total;
        }
        unlock();
    }

    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        e = ESP_FAIL;
    }
    if (e != ESP_OK) {
        esp_https_ota_abort(h);
        snprintf(err, sizeof(err), "Download failed (%s)", esp_err_to_name(e));
        ESP_LOGW(TAG, "%s", err);
        set_state(OTA_STATE_ERROR, err);
        return;
    }
    e = esp_https_ota_finish(h);
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

    /* TLS handshakes are stack-hungry; 8 KB is what esp_https_ota asks for. */
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "updates from github.com/%s, asset %s", ota_repo(), CONFIG_NETDASH_OTA_ASSET);
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
