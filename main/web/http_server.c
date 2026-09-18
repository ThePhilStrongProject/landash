/*
 * NetDash HTTP server: the gzipped single-page UI plus the JSON REST API
 * documented in docs/API.md.
 *
 * Routing: every /api/... endpoint is registered as its own httpd_uri_t (more
 * specific URIs registered before the "/api/devices/" wildcard, since
 * esp_http_server's wildcard matcher picks the first registered handler whose
 * URI matches, regardless of specificity). No handler is registered for "/"
 * or any other path - unmatched requests fall through to the custom 404
 * handler below, which serves the embedded gzipped UI for anything outside
 * "/api/" (this is also how GET / and client-side routing are implemented)
 * and a JSON 404 for anything under "/api/". A custom 405 handler covers the
 * case where esp_http_server finds a matching URI but no handler for the
 * request method.
 */
#include "http_server.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "nvs.h"

#include "app_events.h"
#include "classify.h"
#include "device_db.h"
#include "links.h"
#include "portscan.h"
#include "scanner.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "http_server";

static httpd_handle_t s_server;

/* Embedded gzipped UI, produced by main/CMakeLists.txt from web/www/index.html. */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

/* Request body limit. docs/API.md defines one global rule - "413 | request
 * body over 8 KB" - with no smaller ceiling for any particular endpoint, so
 * every read_body() call below shares this one limit. */
#define BODY_MAX_BYTES 8192

/* ------------------------------------------------------------------------- */
/* Wall-clock validity                                                       */
/* ------------------------------------------------------------------------- */

/*
 * http_server.c needs its own notion of "has NTP synced yet" (for
 * /api/status's time_synced and for is_new) without reaching into
 * device_db's or wifi_mgr's internals, so - like device_db.c does - it
 * subscribes to the same public NETDASH_EVENT_TIME_SYNCED event.
 */
static volatile bool s_time_synced;

static void time_synced_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_time_synced = true;
}

static int64_t now_or_zero(void)
{
    return s_time_synced ? (int64_t)time(NULL) : 0;
}

/* ------------------------------------------------------------------------- */
/* Small formatting / parsing helpers                                       */
/* ------------------------------------------------------------------------- */

static void mac_to_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Strict "aa:bb:cc:dd:ee:ff" parse (case-insensitive on input). */
static bool parse_mac(const char *s, uint8_t out[6])
{
    if (s == NULL || strlen(s) != 17) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int hi = hex_nib(s[i * 3]);
        int lo = hex_nib(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        if (i < 5 && s[i * 3 + 2] != ':') {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* esp_ip4_addr_t::addr is stored the way ESP-IDF's own IP2STR macro reads it:
 * byte 0 (addr & 0xff) is the first dotted-quad octet. */
static void net_ip_to_str(uint32_t addr, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u",
              (unsigned)(addr & 0xff), (unsigned)((addr >> 8) & 0xff),
              (unsigned)((addr >> 16) & 0xff), (unsigned)((addr >> 24) & 0xff));
}

/* device_db.h documents netdash_device_t::ip as "host byte order" - the usual
 * arithmetic convention where the first octet is the most significant byte. */
static void host_ip_to_str(uint32_t ip, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u",
              (unsigned)((ip >> 24) & 0xff), (unsigned)((ip >> 16) & 0xff),
              (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
}

/* ------------------------------------------------------------------------- */
/* JSON response helpers                                                     */
/* ------------------------------------------------------------------------- */

/* Sends obj as the JSON body with the given HTTP status line, then frees obj. */
/*
 * Attaches the full open-port list to a device object. Only the single-device
 * views use it, so the polled list stays small.
 */
static void device_add_ports(cJSON *o, const uint8_t mac[6])
{
    netdash_ports_t ports;
    if (o == NULL || !device_db_get_ports(mac, &ports)) {
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < ports.count; i++) {
        cJSON      *entry   = cJSON_CreateObject();
        const char *service = netdash_port_service(ports.ports[i]);

        cJSON_AddNumberToObject(entry, "port", ports.ports[i]);
        if (service != NULL) {
            cJSON_AddStringToObject(entry, "service", service);
        } else {
            cJSON_AddNullToObject(entry, "service");
        }
        cJSON_AddItemToArray(arr, entry);
    }
    cJSON_AddItemToObject(o, "open_ports", arr);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (out of memory)");
        static const char *oom = "{\"error\":\"out of memory\"}";
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, oom, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, text, strlen(text));
    cJSON_free(text);
    return err;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", msg);
    return send_json(req, status, obj);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    return send_json(req, "200 OK", obj);
}

/* ------------------------------------------------------------------------- */
/* Request body reading                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Reads the whole request body into a freshly malloc'd, NUL-terminated
 * buffer (*out_buf, caller frees). On failure an error response has already
 * been sent (or the recv genuinely failed and the caller must return
 * ESP_FAIL so the server closes the socket, per httpd_req_recv()'s contract).
 */
static esp_err_t read_body(httpd_req_t *req, size_t max_allowed, char **out_buf)
{
    *out_buf = NULL;

    if (req->content_len > max_allowed) {
        send_json_error(req, "413 Content Too Large", "request body too large");
        return ESP_FAIL;
    }

    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        send_json_error(req, "500 Internal Server Error", "out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                /* docs/API.md: every non-2xx response is {"error": ...} and
                 * nothing else, so don't let httpd_resp_send_408()'s default
                 * plain-text body leak out here. */
                send_json_error(req, "408 Request Timeout", "request timeout");
            }
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[req->content_len] = '\0';
    *out_buf = buf;
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* GET / and the catch-all 404 / 405 handlers                                */
/* ------------------------------------------------------------------------- */

static esp_err_t send_index_html(httpd_req_t *req)
{
    /* Every browser sends "Accept-Encoding: gzip", and the UI is only ever
     * embedded gzipped (main/CMakeLists.txt does not keep an uncompressed
     * copy), so this is served unconditionally rather than negotiated. */
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                            (ssize_t)(index_html_gz_end - index_html_gz_start));
}

static bool is_api_path(const char *uri)
{
    return strncmp(uri, "/api/", 5) == 0 || strcmp(uri, "/api") == 0;
}

static esp_err_t err_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (is_api_path(req->uri)) {
        return send_json_error(req, "404 Not Found", "not found");
    }
    return send_index_html(req);
}

static esp_err_t err_405_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return send_json_error(req, "405 Method Not Allowed", "method not allowed");
}

/* ------------------------------------------------------------------------- */
/* GET /api/status                                                           */
/* ------------------------------------------------------------------------- */

static const char *mode_name(wifi_mgr_mode_t mode)
{
    switch (mode) {
    case WIFI_MGR_MODE_STA:   return "sta";
    case WIFI_MGR_MODE_AP:    return "ap";
    case WIFI_MGR_MODE_APSTA: return "apsta";
    default:                  return "off";
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_mgr_mode_t mode = wifi_mgr_get_mode();

    char ip_str[16]      = "0.0.0.0";
    char netmask_str[16] = "0.0.0.0";
    char gw_str[16]      = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    if (wifi_mgr_get_ip_info(&ip_info) == ESP_OK) {
        net_ip_to_str(ip_info.ip.addr, ip_str, sizeof(ip_str));
        net_ip_to_str(ip_info.netmask.addr, netmask_str, sizeof(netmask_str));
        net_ip_to_str(ip_info.gw.addr, gw_str, sizeof(gw_str));
    }

    uint8_t mac[6] = {0};
    wifi_mgr_get_mac(mac);
    char mac_str[18];
    mac_to_str(mac, mac_str);

    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", wifi_mgr_get_ssid());

    netdash_settings_t cfg;
    settings_get(&cfg);

    const esp_app_desc_t *app = esp_app_get_description();

    uint16_t scan_done = 0, scan_total = 0;
    scanner_get_progress(&scan_done, &scan_total);

    size_t total = 0, online = 0, new_24h = 0;
    int64_t now = now_or_zero();
    device_db_lock();
    size_t count = device_db_count();
    for (size_t i = 0; i < count; i++) {
        netdash_device_t d;
        if (!device_db_get_at(i, &d)) {
            break;
        }
        total++;
        if (netdash_device_online(&d)) {
            online++;
        }
        if (d.first_seen != 0 && now != 0 && (now - d.first_seen) < 86400) {
            new_24h++;
        }
    }
    device_db_unlock();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddStringToObject(root, "netmask", netmask_str);
    cJSON_AddStringToObject(root, "gateway", gw_str);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "mode", mode_name(mode));
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddNumberToObject(root, "rssi", wifi_mgr_get_rssi());
    cJSON_AddStringToObject(root, "hostname", cfg.hostname);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min_free", (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "fw", app->version);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddNumberToObject(root, "time", (double)now);
    cJSON_AddBoolToObject(root, "time_synced", s_time_synced);
    cJSON_AddBoolToObject(root, "scanning", scanner_is_running());
    cJSON_AddNumberToObject(root, "scan_done", scan_done);
    cJSON_AddNumberToObject(root, "scan_total", scan_total);
    cJSON_AddNumberToObject(root, "last_sweep", (double)scanner_last_sweep_time());
    cJSON_AddNumberToObject(root, "devices_total", (double)total);
    cJSON_AddNumberToObject(root, "devices_online", (double)online);
    cJSON_AddNumberToObject(root, "devices_new_24h", (double)new_24h);
    if (mode == WIFI_MGR_MODE_AP || mode == WIFI_MGR_MODE_APSTA) {
        cJSON_AddStringToObject(root, "ap_ssid", wifi_mgr_get_ap_ssid());
        cJSON_AddStringToObject(root, "ap_pass", cfg.ap_pass);
    }

    return send_json(req, "200 OK", root);
}

/* ------------------------------------------------------------------------- */
/* The device object + list snapshot helpers                                 */
/* ------------------------------------------------------------------------- */

/* Copies out the "/api/devices/{mac}" tail (before any '?query'), or false
 * when uri does not start with prefix or the tail does not fit out_cap. */
static bool extract_tail(const char *uri, const char *prefix, char *out, size_t out_cap)
{
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) {
        return false;
    }
    const char *tail = uri + plen;
    const char *q     = strchr(tail, '?');
    size_t      len   = q ? (size_t)(q - tail) : strlen(tail);
    if (len >= out_cap) {
        return false;
    }
    memcpy(out, tail, len);
    out[len] = '\0';
    return true;
}

/* Trims leading/trailing spaces and tabs; false when the trimmed string does
 * not fit out_cap (out is left untouched in that case). */
static bool trim_copy(const char *in, char *out, size_t out_cap)
{
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    size_t len = strlen(in);
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\t')) {
        len--;
    }
    if (len >= out_cap) {
        return false;
    }
    memcpy(out, in, len);
    out[len] = '\0';
    return true;
}

static cJSON *device_to_json(const netdash_device_t *d, int64_t now)
{
    char mac_str[18];
    mac_to_str(d->mac, mac_str);
    char ip_str[16];
    host_ip_to_str(d->ip, ip_str, sizeof(ip_str));
    char name[32];
    device_db_display_name(d, name, sizeof(name));

    bool has_override = d->type_override != NETDASH_TYPE_UNKNOWN;
    netdash_type_t effective = has_override ? (netdash_type_t)d->type_override : (netdash_type_t)d->type;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mac", mac_str);
    cJSON_AddStringToObject(o, "ip", ip_str);
    cJSON_AddStringToObject(o, "display_name", name);
    cJSON_AddStringToObject(o, "nickname", d->nickname);
    cJSON_AddStringToObject(o, "hostname", d->hostname);
    cJSON_AddStringToObject(o, "vendor", d->vendor);
    cJSON_AddStringToObject(o, "type", netdash_type_name(effective));
    if (has_override) {
        cJSON_AddStringToObject(o, "type_override", netdash_type_name((netdash_type_t)d->type_override));
    } else {
        cJSON_AddNullToObject(o, "type_override");
    }

    cJSON *services = cJSON_CreateArray();
    for (int b = 0; b < NETDASH_SVC_COUNT; b++) {
        if (d->services & (1u << b)) {
            cJSON_AddItemToArray(services, cJSON_CreateString(netdash_service_name(b)));
        }
    }
    cJSON_AddItemToObject(o, "services", services);

    /* Highest priority first, per docs/API.md. */
    static const uint8_t     src_order[] = {NETDASH_NAME_SRC_MDNS, NETDASH_NAME_SRC_RDNS,
                                             NETDASH_NAME_SRC_NBNS, NETDASH_NAME_SRC_SSDP};
    static const char *const src_names[] = {"mdns", "rdns", "nbns", "ssdp"};
    cJSON *sources = cJSON_CreateArray();
    for (size_t i = 0; i < sizeof(src_order) / sizeof(src_order[0]); i++) {
        if (d->sources & NETDASH_SRC_BIT(src_order[i])) {
            cJSON_AddItemToArray(sources, cJSON_CreateString(src_names[i]));
        }
    }
    cJSON_AddItemToObject(o, "sources", sources);

    cJSON_AddNumberToObject(o, "first_seen", (double)d->first_seen);
    cJSON_AddNumberToObject(o, "last_seen", (double)d->last_seen);
    cJSON_AddBoolToObject(o, "online", netdash_device_online(d));
    bool is_new = d->first_seen != 0 && now != 0 && (now - d->first_seen) < 86400;
    cJSON_AddBoolToObject(o, "is_new", is_new);
    cJSON_AddBoolToObject(o, "hidden", (d->flags & NETDASH_FLAG_HIDDEN) != 0);
    cJSON_AddNumberToObject(o, "rtt_ms", d->rtt_ms);
    cJSON_AddNumberToObject(o, "miss_count", d->miss_count);

    /*
     * Port-scan summary. The list endpoint is polled every few seconds, so it
     * carries counts and progress only; the full port array is attached by
     * device_add_ports() for the single-device view.
     */
    netdash_ports_t ports;
    if (device_db_get_ports(d->mac, &ports)) {
        cJSON_AddNumberToObject(o, "open_port_count", ports.count);
        cJSON_AddNumberToObject(o, "portscan_tier", ports.tier);
        cJSON_AddNumberToObject(o, "portscan_last", (double)ports.last_scan);
        cJSON_AddBoolToObject(o, "portscan_active", ports.scanning_tier != 0);
        if (ports.scanning_tier != 0 && ports.tier_total != 0) {
            cJSON_AddNumberToObject(o, "portscan_done", ports.cursor);
            cJSON_AddNumberToObject(o, "portscan_total", ports.tier_total);
        }
    }
    return o;
}

/*
 * Copies every device out of device_db into a freshly malloc'd array
 * (caller frees), holding device_db_lock() only for the copy loop itself, per
 * CLAUDE.md's "no I/O while holding the lock" rule. *out_n is 0 and the
 * return value is NULL both when the table is empty and when malloc fails;
 * callers distinguish the two with device_db_count().
 */
static netdash_device_t *snapshot_devices(size_t *out_n)
{
    *out_n = 0;
    size_t cap = device_db_count();
    if (cap == 0) {
        return NULL;
    }
    netdash_device_t *buf = malloc(sizeof(netdash_device_t) * cap);
    if (buf == NULL) {
        return NULL;
    }

    device_db_lock();
    size_t n = 0;
    for (size_t i = 0; i < cap; i++) {
        netdash_device_t d;
        if (!device_db_get_at(i, &d)) {
            break;
        }
        buf[n++] = d;
    }
    device_db_unlock();

    *out_n = n;
    return buf;
}

static int cmp_by_ip(const void *a, const void *b)
{
    uint32_t ia = ((const netdash_device_t *)a)->ip;
    uint32_t ib = ((const netdash_device_t *)b)->ip;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* GET /api/devices                                                          */
/* ------------------------------------------------------------------------- */

static esp_err_t devices_list_handler(httpd_req_t *req)
{
    bool show_hidden   = false;
    int  online_filter = -1; /* -1 = no filter */

    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "hidden", val, sizeof(val)) == ESP_OK) {
            show_hidden = (val[0] == '1');
        }
        if (httpd_query_key_value(query, "online", val, sizeof(val)) == ESP_OK) {
            online_filter = (val[0] == '1') ? 1 : 0;
        }
    }

    size_t             n   = 0;
    netdash_device_t  *buf = snapshot_devices(&n);
    if (n == 0 && device_db_count() > 0 && buf == NULL) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    if (buf != NULL) {
        qsort(buf, n, sizeof(*buf), cmp_by_ip);
    }

    int64_t now = now_or_zero();
    cJSON  *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        netdash_device_t *d      = &buf[i];
        bool               hidden = (d->flags & NETDASH_FLAG_HIDDEN) != 0;
        if (hidden && !show_hidden) {
            continue;
        }
        bool online = netdash_device_online(d);
        if ((online_filter == 1 && !online) || (online_filter == 0 && online)) {
            continue;
        }
        cJSON_AddItemToArray(arr, device_to_json(d, now));
    }
    free(buf);

    ESP_LOGD(TAG, "devices list: %u of %u, free heap %u bytes",
              (unsigned)cJSON_GetArraySize(arr), (unsigned)n, (unsigned)esp_get_free_heap_size());
    return send_json(req, "200 OK", arr);
}

/* ------------------------------------------------------------------------- */
/* GET /api/devices/export (must be routed before "/api/devices/")           */
/* ------------------------------------------------------------------------- */

static esp_err_t devices_export_handler(httpd_req_t *req)
{
    size_t             n   = 0;
    netdash_device_t  *buf = snapshot_devices(&n);
    if (n == 0 && device_db_count() > 0 && buf == NULL) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    if (buf != NULL) {
        qsort(buf, n, sizeof(*buf), cmp_by_ip);
    }

    netdash_settings_t cfg;
    settings_get(&cfg);

    int64_t now = now_or_zero();
    cJSON  *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, device_to_json(&buf[i], now));
    }
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "exported_at", (double)now);
    cJSON_AddStringToObject(root, "hostname", cfg.hostname);
    cJSON_AddItemToObject(root, "devices", arr);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"netdash-devices.json\"");
    esp_err_t err = httpd_resp_send(req, text, strlen(text));
    cJSON_free(text);
    return err;
}

/* ------------------------------------------------------------------------- */
/* POST /api/devices/import (must be routed before "/api/devices/")          */
/* ------------------------------------------------------------------------- */

/*
 * Parses the optional nickname/type_override/hidden members of a device
 * import/patch entry into device_db_set_user()'s argument shape.
 * nickname_buf must be at least 32 bytes. Returns false only on a malformed
 * "type_override" or "hidden" member when strict is true (used by PATCH,
 * which must 400 on bad input); import (strict = false) just ignores members
 * it cannot use, per docs/API.md's "every field ... is ignored" language.
 */
static bool parse_user_fields(cJSON *obj, bool strict, const char **out_nickname,
                                char *nickname_buf, int *out_type_override, int *out_flags,
                                const char **err_msg)
{
    *out_nickname      = NULL;
    *out_type_override = -1;
    *out_flags          = -1;

    cJSON *j_nick = cJSON_GetObjectItemCaseSensitive(obj, "nickname");
    if (j_nick != NULL) {
        if (cJSON_IsString(j_nick) && trim_copy(j_nick->valuestring, nickname_buf, 32)) {
            *out_nickname = nickname_buf;
        } else if (strict) {
            *err_msg = cJSON_IsString(j_nick) ? "nickname too long" : "nickname must be a string";
            return false;
        }
    }

    cJSON *j_type = cJSON_GetObjectItemCaseSensitive(obj, "type_override");
    if (j_type != NULL) {
        if (cJSON_IsNull(j_type)) {
            *out_type_override = NETDASH_TYPE_UNKNOWN;
        } else if (cJSON_IsString(j_type)) {
            netdash_type_t match = NETDASH_TYPE_MAX;
            for (int t = 0; t < NETDASH_TYPE_MAX; t++) {
                if (strcmp(netdash_type_name((netdash_type_t)t), j_type->valuestring) == 0) {
                    match = (netdash_type_t)t;
                    break;
                }
            }
            if (match != NETDASH_TYPE_MAX) {
                *out_type_override = match;
            } else if (strict) {
                *err_msg = "unknown type_override";
                return false;
            }
        } else if (strict) {
            *err_msg = "type_override must be a string or null";
            return false;
        }
    }

    cJSON *j_hidden = cJSON_GetObjectItemCaseSensitive(obj, "hidden");
    if (j_hidden != NULL) {
        if (cJSON_IsBool(j_hidden)) {
            *out_flags = cJSON_IsTrue(j_hidden) ? NETDASH_FLAG_HIDDEN : 0;
        } else if (strict) {
            *err_msg = "hidden must be a boolean";
            return false;
        }
    }

    return true;
}

static esp_err_t devices_import_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_json_error(req, "400 Bad Request", "malformed json");
    }

    cJSON *j_ver     = cJSON_GetObjectItemCaseSensitive(json, "version");
    cJSON *j_devices = cJSON_GetObjectItemCaseSensitive(json, "devices");
    if (!cJSON_IsNumber(j_ver) || j_ver->valueint != 1 || !cJSON_IsArray(j_devices)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "expected version 1 and a devices array");
    }

    int    imported = 0;
    int    skipped  = 0;
    cJSON *entry;
    cJSON_ArrayForEach(entry, j_devices) {
        uint8_t mac[6];
        cJSON  *j_mac = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "mac") : NULL;
        if (!cJSON_IsString(j_mac) || !parse_mac(j_mac->valuestring, mac)) {
            skipped++;
            continue;
        }
        if (device_db_ensure(mac) != ESP_OK) {
            skipped++;
            continue;
        }

        const char *nickname = NULL;
        char        nickname_buf[32];
        int         type_override, flags;
        const char *unused_err = NULL;
        parse_user_fields(entry, false, &nickname, nickname_buf, &type_override, &flags, &unused_err);

        if (device_db_set_user(mac, nickname, type_override, flags) == ESP_OK) {
            imported++;
        } else {
            skipped++;
        }
    }
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "imported", imported);
    cJSON_AddNumberToObject(resp, "skipped", skipped);
    return send_json(req, "200 OK", resp);
}

/* ------------------------------------------------------------------------- */
/* GET / PATCH / DELETE /api/devices/{mac}                                   */
/* ------------------------------------------------------------------------- */

static esp_err_t devices_get_handler(httpd_req_t *req)
{
    char tail[40];
    if (!extract_tail(req->uri, "/api/devices/", tail, sizeof(tail))) {
        return send_json_error(req, "404 Not Found", "not found");
    }
    uint8_t mac[6];
    if (!parse_mac(tail, mac)) {
        return send_json_error(req, "400 Bad Request", "bad mac");
    }
    netdash_device_t d;
    if (!device_db_get_by_mac(mac, &d)) {
        return send_json_error(req, "404 Not Found", "device not found");
    }
    cJSON *o = device_to_json(&d, now_or_zero());
    device_add_ports(o, mac);
    return send_json(req, "200 OK", o);
}

/*
 * POST /api/devices/{mac}/portscan - forget this device's results and put it
 * at the head of the queue.
 */
static esp_err_t devices_portscan_handler(httpd_req_t *req)
{
    char tail[48];
    if (!extract_tail(req->uri, "/api/devices/", tail, sizeof(tail))) {
        return send_json_error(req, "404 Not Found", "not found");
    }

    char *slash = strchr(tail, '/');
    if (slash == NULL || strcmp(slash, "/portscan") != 0) {
        return send_json_error(req, "404 Not Found", "not found");
    }
    *slash = '\0';

    uint8_t mac[6];
    if (!parse_mac(tail, mac)) {
        return send_json_error(req, "400 Bad Request", "bad mac");
    }
    netdash_device_t d;
    if (!device_db_get_by_mac(mac, &d)) {
        return send_json_error(req, "404 Not Found", "device not found");
    }

    const esp_err_t err = portscan_rescan_device(mac);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "rescan failed");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "queued", tail);
    return send_json(req, "200 OK", o);
}

/* GET /api/portscan - overall scanner progress. */
static esp_err_t portscan_status_handler(httpd_req_t *req)
{
    portscan_status_t st;
    portscan_get_status(&st);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", st.enabled);
    cJSON_AddBoolToObject(o, "running", st.running);
    cJSON_AddNumberToObject(o, "tier", st.tier);
    cJSON_AddNumberToObject(o, "ceiling", st.ceiling);
    cJSON_AddNumberToObject(o, "max_tier", st.max_tier);
    cJSON_AddNumberToObject(o, "rate", st.rate);
    cJSON_AddNumberToObject(o, "device_index", st.device_index);
    cJSON_AddNumberToObject(o, "device_count", st.device_count);
    cJSON_AddNumberToObject(o, "cursor", st.cursor);
    cJSON_AddNumberToObject(o, "tier_total", st.tier_total);
    cJSON_AddNumberToObject(o, "probes", st.probes);
    cJSON_AddNumberToObject(o, "found", st.found);
    cJSON_AddNumberToObject(o, "cycle_started", (double)st.cycle_started);
    return send_json(req, "200 OK", o);
}

static esp_err_t devices_patch_handler(httpd_req_t *req)
{
    char tail[40];
    if (!extract_tail(req->uri, "/api/devices/", tail, sizeof(tail))) {
        return send_json_error(req, "404 Not Found", "not found");
    }
    uint8_t mac[6];
    if (!parse_mac(tail, mac)) {
        return send_json_error(req, "400 Bad Request", "bad mac");
    }

    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_json_error(req, "400 Bad Request", "malformed json");
    }
    netdash_device_t existing;
    if (!device_db_get_by_mac(mac, &existing)) {
        cJSON_Delete(json);
        return send_json_error(req, "404 Not Found", "device not found");
    }

    const char *nickname = NULL;
    char        nickname_buf[32];
    int         type_override, flags;
    const char *err_msg = NULL;
    if (!parse_user_fields(json, true, &nickname, nickname_buf, &type_override, &flags, &err_msg)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", err_msg);
    }
    cJSON_Delete(json);

    esp_err_t err = device_db_set_user(mac, nickname, type_override, flags);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "device not found");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "failed to save device");
    }

    netdash_device_t updated;
    if (!device_db_get_by_mac(mac, &updated)) {
        return send_json_error(req, "404 Not Found", "device not found");
    }
    cJSON *out = device_to_json(&updated, now_or_zero());
    device_add_ports(out, mac);
    return send_json(req, "200 OK", out);
}

static esp_err_t devices_delete_handler(httpd_req_t *req)
{
    char tail[40];
    if (!extract_tail(req->uri, "/api/devices/", tail, sizeof(tail))) {
        return send_json_error(req, "404 Not Found", "not found");
    }
    uint8_t mac[6];
    if (!parse_mac(tail, mac)) {
        return send_json_error(req, "400 Bad Request", "bad mac");
    }
    esp_err_t err = device_db_remove(mac);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "device not found");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "failed to remove device");
    }
    return send_ok(req);
}

/* ------------------------------------------------------------------------- */
/* POST /api/scan                                                            */
/* ------------------------------------------------------------------------- */

/*
 * docs/API.md (authoritative) has this return 200 with "scanning": true, both
 * when a sweep was just started and when one was already running - it is
 * explicitly a no-op, not an error, in the latter case.
 */
static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!scanner_is_running()) {
        netdash_settings_t cfg;
        settings_get(&cfg);
        esp_netif_ip_info_t ip_info;
        bool has_ip = wifi_mgr_get_ip_info(&ip_info) == ESP_OK;
        if (cfg.passive_only || !has_ip) {
            return send_json_error(req, "503 Service Unavailable", "wifi not ready");
        }
        scanner_trigger_now();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "scanning", true);
    return send_json(req, "200 OK", resp);
}

/* ------------------------------------------------------------------------- */
/* GET /api/events                                                           */
/* ------------------------------------------------------------------------- */

static esp_err_t events_handler(httpd_req_t *req)
{
    int limit = 100;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK) {
            int v = atoi(val);
            if (v >= 1 && v <= 100) {
                limit = v;
            }
        }
    }

    size_t total = events_log_count();
    size_t n     = ((size_t)limit < total) ? (size_t)limit : total;

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        netdash_event_rec_t rec;
        if (!events_log_get(i, &rec)) {
            break;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", (double)rec.ts);
        cJSON_AddStringToObject(o, "type", netdash_log_type_name((netdash_log_type_t)rec.type));

        static const uint8_t zero_mac[6] = {0};
        if (memcmp(rec.mac, zero_mac, 6) != 0) {
            char mac_str[18];
            mac_to_str(rec.mac, mac_str);
            cJSON_AddStringToObject(o, "mac", mac_str);
        } else {
            cJSON_AddNullToObject(o, "mac");
        }
        if (rec.ip != 0) {
            char ip_str[16];
            host_ip_to_str(rec.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(o, "ip", ip_str);
        } else {
            cJSON_AddNullToObject(o, "ip");
        }
        cJSON_AddStringToObject(o, "text", rec.text);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, "200 OK", arr);
}

/* ------------------------------------------------------------------------- */
/* GET / PUT /api/settings                                                   */
/* ------------------------------------------------------------------------- */

static cJSON *settings_to_json(const netdash_settings_t *cfg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddBoolToObject(o, "wifi_configured", cfg->wifi_ssid[0] != '\0');
    cJSON_AddStringToObject(o, "hostname", cfg->hostname);
    cJSON_AddStringToObject(o, "ap_pass", cfg->ap_pass);
    cJSON_AddNumberToObject(o, "scan_interval_min", cfg->scan_interval_min);
    cJSON_AddNumberToObject(o, "hosts_per_sec", cfg->hosts_per_sec);
    cJSON_AddBoolToObject(o, "passive_only", cfg->passive_only);
    cJSON_AddStringToObject(o, "tz", cfg->tz);
    cJSON_AddStringToObject(o, "ntp_server", cfg->ntp_server);
    cJSON_AddBoolToObject(o, "portscan_enabled", cfg->portscan_enabled);
    cJSON_AddNumberToObject(o, "portscan_rate", cfg->portscan_rate);
    cJSON_AddNumberToObject(o, "portscan_max_tier", cfg->portscan_max_tier);
    return o;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    netdash_settings_t cfg;
    settings_get(&cfg);
    return send_json(req, "200 OK", settings_to_json(&cfg));
}

/* RFC-1123 label: 1-31 chars of a-z 0-9 -, not starting/ending with '-'. */
static bool valid_hostname(const char *s)
{
    size_t len = strlen(s);
    if (len < 1 || len > 31 || s[0] == '-' || s[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t settings_put_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_json_error(req, "400 Bad Request", "malformed json");
    }

    netdash_settings_t cur;
    settings_get(&cur);
    netdash_settings_t next = cur;
    bool ssid_changed = false;
    bool pass_changed = false;
    cJSON *j;

    j = cJSON_GetObjectItemCaseSensitive(json, "wifi_ssid");
    if (j != NULL) {
        if (!cJSON_IsString(j) || strlen(j->valuestring) > 32) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "wifi_ssid must be a string up to 32 chars");
        }
        snprintf(next.wifi_ssid, sizeof(next.wifi_ssid), "%s", j->valuestring);
        ssid_changed = strcmp(next.wifi_ssid, cur.wifi_ssid) != 0;
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "wifi_pass");
    if (j != NULL) {
        if (!cJSON_IsString(j) || strlen(j->valuestring) > 64) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "wifi_pass must be a string up to 64 chars");
        }
        /* "" means unchanged, per docs/API.md - clear the SSID to actually clear the password. */
        if (j->valuestring[0] != '\0') {
            pass_changed = strcmp(j->valuestring, cur.wifi_pass) != 0;
            snprintf(next.wifi_pass, sizeof(next.wifi_pass), "%s", j->valuestring);
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "hostname");
    if (j != NULL) {
        if (!cJSON_IsString(j) || !valid_hostname(j->valuestring)) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "hostname must be 1-31 chars of a-z, 0-9, -");
        }
        snprintf(next.hostname, sizeof(next.hostname), "%s", j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "ap_pass");
    if (j != NULL) {
        size_t len = cJSON_IsString(j) ? strlen(j->valuestring) : 0;
        if (!cJSON_IsString(j) || len < 8 || len > 63) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "ap_pass must be 8-63 chars");
        }
        snprintf(next.ap_pass, sizeof(next.ap_pass), "%s", j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "scan_interval_min");
    if (j != NULL) {
        if (!cJSON_IsNumber(j) || j->valueint < 1 || j->valueint > 1440) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "scan_interval_min must be 1-1440");
        }
        next.scan_interval_min = (uint16_t)j->valueint;
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "hosts_per_sec");
    if (j != NULL) {
        if (!cJSON_IsNumber(j) || j->valueint < 1 || j->valueint > 64) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "hosts_per_sec must be 1-64");
        }
        next.hosts_per_sec = (uint16_t)j->valueint;
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "passive_only");
    if (j != NULL) {
        if (!cJSON_IsBool(j)) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "passive_only must be a boolean");
        }
        next.passive_only = cJSON_IsTrue(j);
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "portscan_enabled");
    if (j != NULL) {
        if (!cJSON_IsBool(j)) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "portscan_enabled must be a boolean");
        }
        next.portscan_enabled = cJSON_IsTrue(j);
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "portscan_rate");
    if (j != NULL) {
        if (!cJSON_IsNumber(j) || j->valueint < 1 || j->valueint > 200) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "portscan_rate must be 1-200");
        }
        next.portscan_rate = (uint16_t)j->valueint;
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "portscan_max_tier");
    if (j != NULL) {
        if (!cJSON_IsNumber(j) || j->valueint < 1 || j->valueint > 3) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "portscan_max_tier must be 1-3");
        }
        next.portscan_max_tier = (uint8_t)j->valueint;
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "tz");
    if (j != NULL) {
        if (!cJSON_IsString(j) || strlen(j->valuestring) > 47) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "tz must be a string up to 47 chars");
        }
        snprintf(next.tz, sizeof(next.tz), "%s", j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(json, "ntp_server");
    if (j != NULL) {
        if (!cJSON_IsString(j) || strlen(j->valuestring) > 63) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "ntp_server must be a string up to 63 chars");
        }
        snprintf(next.ntp_server, sizeof(next.ntp_server), "%s", j->valuestring);
    }

    cJSON_Delete(json);

    esp_err_t err = settings_set(&next);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "failed to save settings");
    }
    /* Enqueues and returns immediately; the actual reconnect happens after
     * this handler has replied, which is exactly what lets the UI catch the
     * "reconnecting" hint and start polling /api/status for the new IP. */
    wifi_mgr_apply_settings();
    portscan_settings_changed();

    netdash_settings_t saved;
    settings_get(&saved);
    cJSON *resp = settings_to_json(&saved);
    cJSON_AddBoolToObject(resp, "reconnecting", ssid_changed || pass_changed);
    return send_json(req, "200 OK", resp);
}

/* ------------------------------------------------------------------------- */
/* POST /api/wifi/scan                                                       */
/* ------------------------------------------------------------------------- */

#define WIFI_SCAN_MAX 24

static const char *auth_name(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2_wpa3_psk";
    default:                        return "unknown";
    }
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    wifi_mgr_ap_record_t *list = malloc(sizeof(wifi_mgr_ap_record_t) * WIFI_SCAN_MAX);
    if (list == NULL) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }

    int n = wifi_mgr_scan(list, WIFI_SCAN_MAX);
    if (n < 0) {
        free(list);
        return send_json_error(req, "503 Service Unavailable", "wifi scan failed");
    }

    /* wifi_mgr_scan() is documented strongest-first; collapse duplicate
     * non-empty SSIDs here (keeping the strongest, i.e. first, occurrence) to
     * meet docs/API.md's "duplicates collapsed". Hidden networks (ssid "")
     * are left as separate rows since they cannot be meaningfully merged. */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (list[i].ssid[0] != '\0') {
            bool dup = false;
            for (int k = 0; k < i; k++) {
                if (list[k].ssid[0] != '\0' && strcmp(list[k].ssid, list[i].ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", list[i].rssi);
        cJSON_AddStringToObject(o, "auth", auth_name(list[i].authmode));
        cJSON_AddNumberToObject(o, "channel", list[i].channel);
        cJSON_AddItemToArray(arr, o);
    }
    free(list);
    return send_json(req, "200 OK", arr);
}

/* ------------------------------------------------------------------------- */
/* POST /api/system/reboot and /api/system/factory-reset                     */
/* ------------------------------------------------------------------------- */

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* Schedules esp_restart() after delay_us so the handler's response has time
 * to reach the client first. */
static void schedule_restart(uint64_t delay_us)
{
    esp_timer_handle_t           timer;
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name     = "netdash_reboot",
    };
    if (esp_timer_create(&args, &timer) == ESP_OK && esp_timer_start_once(timer, delay_us) == ESP_OK) {
        return;
    }
    ESP_LOGE(TAG, "failed to schedule restart, rebooting immediately");
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    esp_err_t err = send_ok(req);
    schedule_restart(500000);
    return err;
}

static void erase_nvs_namespace(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to erase NVS namespace '%s': %s", ns, esp_err_to_name(err));
    }
    nvs_close(h);
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *json      = cJSON_Parse(body);
    free(body);
    cJSON *j_confirm = json != NULL ? cJSON_GetObjectItemCaseSensitive(json, "confirm") : NULL;
    bool   confirmed = cJSON_IsString(j_confirm) && strcmp(j_confirm->valuestring, "factory-reset") == 0;
    if (json != NULL) {
        cJSON_Delete(json);
    }
    if (!confirmed) {
        return send_json_error(req, "400 Bad Request", "confirmation required");
    }

    esp_err_t err = send_ok(req);
    settings_clear_wifi();
    erase_nvs_namespace("dev");
    erase_nvs_namespace("cfg");
    schedule_restart(500000);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Registration + lifecycle                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Order matters for the two "/api/devices/" GET routes: esp_http_server's
 * wildcard matcher (see httpd_find_uri_handler() in esp_http_server) returns
 * the first registered handler whose URI and method both match, not the
 * most specific one. "/api/devices/export" (GET) must therefore be
 * registered before the "/api/devices/" wildcard GET handler, or every export
 * request would be routed to devices_get_handler() and fail MAC parsing.
 * "/api/devices/import" only needs to precede the wildcard PATCH/DELETE
 * entries in principle, but is kept alongside export for clarity - POST is
 * not one of the wildcard's registered methods, so there is no actual
 * ambiguity there.
 */
/* ------------------------------------------------------------------------- */
/* Dashboard quick links                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Builds one link object, resolving the address from the device table on every
 * call. That resolution is the whole point: a link is stored against a MAC, so
 * it keeps working when DHCP hands the device a different address.
 */
static cJSON *link_to_json(const netdash_link_t *l)
{
    cJSON *o = cJSON_CreateObject();
    char   mac_str[18];
    mac_to_str(l->mac, mac_str);

    cJSON_AddNumberToObject(o, "id", l->id);
    cJSON_AddStringToObject(o, "mac", mac_str);
    cJSON_AddNumberToObject(o, "port", l->port);
    cJSON_AddStringToObject(o, "scheme", netdash_scheme_name((netdash_scheme_t)l->scheme));

    netdash_device_t dev;
    const bool       known = device_db_get_by_mac(l->mac, &dev);

    char name[32];
    name[0] = 0;
    if (known) {
        device_db_display_name(&dev, name, sizeof(name));
    }

    const netdash_type_t type = known
        ? (netdash_type_t)(dev.type_override != 0 ? dev.type_override : dev.type)
        : NETDASH_TYPE_UNKNOWN;

    cJSON_AddStringToObject(o, "label", l->label[0] != 0 ? l->label : name);
    cJSON_AddStringToObject(o, "display_name", name);
    cJSON_AddStringToObject(o, "type", netdash_type_name(type));
    cJSON_AddBoolToObject(o, "online", known && netdash_device_online(&dev));

    char ip_str[16];
    strcpy(ip_str, "0.0.0.0");
    if (known && dev.ip != 0) {
        host_ip_to_str(dev.ip, ip_str, sizeof(ip_str));
    }
    cJSON_AddStringToObject(o, "ip", ip_str);

    if (known && dev.ip != 0) {
        char url[48];
        snprintf(url, sizeof(url), "%s://%s:%u",
                 netdash_scheme_name((netdash_scheme_t)l->scheme), ip_str,
                 (unsigned)l->port);
        cJSON_AddStringToObject(o, "url", url);
    } else {
        cJSON_AddNullToObject(o, "url");
    }
    return o;
}

static cJSON *links_array(void)
{
    cJSON       *arr = cJSON_CreateArray();
    const size_t n   = links_count();

    for (size_t i = 0; i < n; i++) {
        netdash_link_t l;
        if (links_get_at(i, &l)) {
            cJSON_AddItemToArray(arr, link_to_json(&l));
        }
    }
    return arr;
}

static esp_err_t links_get_handler(httpd_req_t *req)
{
    return send_json(req, "200 OK", links_array());
}

/* Accepts "http" or "https". Returns -1 for anything else. */
static int parse_scheme(const char *s)
{
    if (s == NULL) {
        return -1;
    }
    if (strcmp(s, "http") == 0) {
        return (int)NETDASH_SCHEME_HTTP;
    }
    if (strcmp(s, "https") == 0) {
        return (int)NETDASH_SCHEME_HTTPS;
    }
    return -1;
}

static esp_err_t links_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *j_mac = cJSON_GetObjectItemCaseSensitive(json, "mac");
    uint8_t      mac[6];
    if (!cJSON_IsString(j_mac) || !parse_mac(j_mac->valuestring, mac)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "bad mac");
    }

    const cJSON *j_port = cJSON_GetObjectItemCaseSensitive(json, "port");
    if (!cJSON_IsNumber(j_port) || j_port->valueint < 1 || j_port->valueint > 65535) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "port must be 1-65535");
    }
    const uint16_t port = (uint16_t)j_port->valueint;

    int          scheme   = (int)links_default_scheme(port);
    const cJSON *j_scheme = cJSON_GetObjectItemCaseSensitive(json, "scheme");
    if (j_scheme != NULL) {
        scheme = cJSON_IsString(j_scheme) ? parse_scheme(j_scheme->valuestring) : -1;
        if (scheme < 0) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "scheme must be http or https");
        }
    }

    netdash_device_t dev;
    if (!device_db_get_by_mac(mac, &dev)) {
        cJSON_Delete(json);
        return send_json_error(req, "404 Not Found", "device not found");
    }

    /* Default the label to whatever the device is currently called. */
    char         label[NETDASH_LINK_LABEL];
    const cJSON *j_label = cJSON_GetObjectItemCaseSensitive(json, "label");
    label[0] = 0;
    if (cJSON_IsString(j_label) && j_label->valuestring[0] != 0) {
        /* Reject rather than truncate, to match PATCH. Silently storing a
           shortened label would leave the UI showing something the caller
           never asked for. */
        if (strlen(j_label->valuestring) >= NETDASH_LINK_LABEL) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "label too long");
        }
        strncpy(label, j_label->valuestring, sizeof(label) - 1);
        label[sizeof(label) - 1] = 0;
    } else {
        device_db_display_name(&dev, label, sizeof(label));
    }
    cJSON_Delete(json);

    uint16_t        id  = 0;
    const esp_err_t err = links_add(mac, port, (netdash_scheme_t)scheme, label, &id);
    if (err == ESP_ERR_NO_MEM) {
        return send_json_error(req, "409 Conflict", "dashboard is full");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not add link");
    }

    netdash_link_t created;
    if (!links_get_by_id(id, &created)) {
        return send_json_error(req, "500 Internal Server Error", "link vanished");
    }
    return send_json(req, "200 OK", link_to_json(&created));
}

/* PUT /api/links - set the order, dropping anything not listed. */
static esp_err_t links_put_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL || !cJSON_IsArray(json)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "expected an array");
    }

    uint16_t ids[NETDASH_MAX_LINKS];
    size_t   n = 0;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (n >= NETDASH_MAX_LINKS) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "too many links");
        }
        const cJSON *j_id = cJSON_IsObject(item)
                                ? cJSON_GetObjectItemCaseSensitive(item, "id")
                                : item;
        if (!cJSON_IsNumber(j_id) || j_id->valueint < 0 || j_id->valueint > 65534) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "bad id");
        }
        ids[n++] = (uint16_t)j_id->valueint;
    }
    cJSON_Delete(json);

    if (links_reorder(ids, n) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "unknown or duplicate id");
    }
    return send_json(req, "200 OK", links_array());
}

/* Pulls the numeric id out of /api/links/{id}. UINT16_MAX when malformed. */
static uint16_t link_id_from_uri(const char *uri)
{
    char tail[24];
    if (!extract_tail(uri, "/api/links/", tail, sizeof(tail))) {
        return UINT16_MAX;
    }
    char      *end = NULL;
    const long v   = strtol(tail, &end, 10);
    if (end == tail || end == NULL || *end != 0 || v < 0 || v > 65534) {
        return UINT16_MAX;
    }
    return (uint16_t)v;
}

static esp_err_t links_patch_handler(httpd_req_t *req)
{
    const uint16_t id = link_id_from_uri(req->uri);
    if (id == UINT16_MAX) {
        return send_json_error(req, "400 Bad Request", "bad id");
    }

    char *body = NULL;
    if (read_body(req, BODY_MAX_BYTES, &body) != ESP_OK) {
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const char  *label = NULL;
    const cJSON *j     = cJSON_GetObjectItemCaseSensitive(json, "label");
    if (j != NULL) {
        if (!cJSON_IsString(j) || strlen(j->valuestring) >= NETDASH_LINK_LABEL) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "label too long");
        }
        label = j->valuestring;
    }

    uint16_t port = UINT16_MAX;
    j             = cJSON_GetObjectItemCaseSensitive(json, "port");
    if (j != NULL) {
        if (!cJSON_IsNumber(j) || j->valueint < 1 || j->valueint > 65535) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "port must be 1-65535");
        }
        port = (uint16_t)j->valueint;
    }

    int scheme = -1;
    j          = cJSON_GetObjectItemCaseSensitive(json, "scheme");
    if (j != NULL) {
        scheme = cJSON_IsString(j) ? parse_scheme(j->valuestring) : -1;
        if (scheme < 0) {
            cJSON_Delete(json);
            return send_json_error(req, "400 Bad Request", "scheme must be http or https");
        }
    }

    const esp_err_t err = links_update(id, label, port, scheme);
    cJSON_Delete(json);

    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "link not found");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not update link");
    }

    netdash_link_t updated;
    if (!links_get_by_id(id, &updated)) {
        return send_json_error(req, "404 Not Found", "link not found");
    }
    return send_json(req, "200 OK", link_to_json(&updated));
}

static esp_err_t links_delete_handler(httpd_req_t *req)
{
    const uint16_t id = link_id_from_uri(req->uri);
    if (id == UINT16_MAX) {
        return send_json_error(req, "400 Bad Request", "bad id");
    }
    if (links_remove(id) != ESP_OK) {
        return send_json_error(req, "404 Not Found", "link not found");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, "200 OK", o);
}

static const httpd_uri_t s_uri_handlers[] = {
    {.uri = "/api/status",               .method = HTTP_GET,    .handler = status_handler},
    {.uri = "/api/devices",              .method = HTTP_GET,    .handler = devices_list_handler},
    {.uri = "/api/devices/export",       .method = HTTP_GET,    .handler = devices_export_handler},
    {.uri = "/api/devices/import",       .method = HTTP_POST,   .handler = devices_import_handler},
    {.uri = "/api/devices/*/portscan",   .method = HTTP_POST,   .handler = devices_portscan_handler},
    {.uri = "/api/portscan",             .method = HTTP_GET,    .handler = portscan_status_handler},
    {.uri = "/api/links",                .method = HTTP_GET,    .handler = links_get_handler},
    {.uri = "/api/links",                .method = HTTP_POST,   .handler = links_post_handler},
    {.uri = "/api/links",                .method = HTTP_PUT,    .handler = links_put_handler},
    {.uri = "/api/links/*",              .method = HTTP_PATCH,  .handler = links_patch_handler},
    {.uri = "/api/links/*",              .method = HTTP_DELETE, .handler = links_delete_handler},
    {.uri = "/api/devices/*",            .method = HTTP_GET,    .handler = devices_get_handler},
    {.uri = "/api/devices/*",            .method = HTTP_PATCH,  .handler = devices_patch_handler},
    {.uri = "/api/devices/*",            .method = HTTP_DELETE, .handler = devices_delete_handler},
    {.uri = "/api/scan",                 .method = HTTP_POST,   .handler = scan_handler},
    {.uri = "/api/events",               .method = HTTP_GET,    .handler = events_handler},
    {.uri = "/api/settings",             .method = HTTP_GET,    .handler = settings_get_handler},
    {.uri = "/api/settings",             .method = HTTP_PUT,    .handler = settings_put_handler},
    {.uri = "/api/wifi/scan",            .method = HTTP_POST,   .handler = wifi_scan_handler},
    {.uri = "/api/system/reboot",        .method = HTTP_POST,   .handler = reboot_handler},
    {.uri = "/api/system/factory-reset", .method = HTTP_POST,   .handler = factory_reset_handler},
};

esp_err_t http_server_init(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    /* Best-effort: if this fails, /api/status.time_synced and is_new just
     * stay pinned to "not synced" until a later subscribe would help - not
     * fatal to the server itself. */
    esp_err_t reg_err = esp_event_handler_register(NETDASH_EVENT, NETDASH_EVENT_TIME_SYNCED,
                                                     time_synced_handler, NULL);
    if (reg_err != ESP_OK) {
        ESP_LOGW(TAG, "could not subscribe to TIME_SYNCED: %s", esp_err_to_name(reg_err));
    }

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = NETDASH_HTTPD_PORT;
    config.max_uri_handlers = NETDASH_HTTPD_MAX_URI_HANDLERS;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;
    /* Leaves room in the lwIP socket budget for the port scanner's batch. */
    config.max_open_sockets = 5;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, err_404_handler);
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED, err_405_handler);

    for (size_t i = 0; i < sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]); i++) {
        err = httpd_register_uri_handler(s_server, &s_uri_handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s (method %d): %s",
                      s_uri_handlers[i].uri, (int)s_uri_handlers[i].method, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "listening on port %u", (unsigned)NETDASH_HTTPD_PORT);
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server       = NULL;
    return err;
}
