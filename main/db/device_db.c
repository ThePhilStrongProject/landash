/*
 * Device database: a fixed-size RAM table of every host seen on the LAN, the
 * events ring buffer, and the NVS-backed persistence of the user-owned
 * fields (nickname / type override / hidden flag).
 *
 * Locking: device_db_lock()/unlock() wrap a FreeRTOS recursive mutex around
 * the RAM table. Every public function except device_db_get_at() takes the
 * lock internally and never performs NVS I/O or posts events while holding
 * it - mutations build a small snapshot under the lock, release it, then do
 * I/O and event posting against the snapshot. The events ring buffer has its
 * own, independent mutex.
 *
 * NVS: namespace "dev", key = 12 lowercase hex chars of the MAC, value a
 * packed blob {version, nickname[32], type_override, flags, first_seen,
 * last_ip}. Only the user-owned fields and first_seen are persisted; live
 * telemetry (ip/hostname/vendor/services/last_seen/rtt/miss_count) is not.
 */
#include "device_db.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_events.h"
#include "classify.h"
#include "oui.h"
#include "wifi_mgr.h"

static const char *TAG = "device_db";

/* ------------------------------------------------------------------------- */
/* RAM table                                                                  */
/* ------------------------------------------------------------------------- */

static netdash_device_t  s_devices[NETDASH_MAX_DEVICES];
static bool              s_seen[NETDASH_MAX_DEVICES];      /* seen this sweep */
static bool              s_persisted[NETDASH_MAX_DEVICES]; /* has an NVS blob */
/* Parallel to s_devices[]; see the comment on netdash_ports_t in the header. */
static netdash_ports_t   s_ports[NETDASH_MAX_DEVICES];
static size_t            s_count;
static SemaphoreHandle_t s_lock;

/* Scratch buffer for device_db_mark_sweep_end(); only that function touches
 * it, and only the scanner task calls it, so no extra lock is needed. */
static uint8_t s_offline_macs[NETDASH_MAX_DEVICES][6];

/* ------------------------------------------------------------------------- */
/* Wall-clock helper                                                         */
/* ------------------------------------------------------------------------- */

/*
 * events_log_push() has no `now` parameter, so it needs its own idea of
 * whether the clock is trustworthy yet. Subscribing to NETDASH_EVENT_TIME_
 * SYNCED (posted once by wifi_mgr/SNTP) avoids duplicating that logic here.
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
/* Locking                                                                    */
/* ------------------------------------------------------------------------- */

void device_db_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void device_db_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Defined with the rest of the port-scan code further down. */
static void ports_restore_locked(int idx);

/* Caller must hold device_db_lock(). Returns -1 when mac is not present. */
static int find_index_locked(const uint8_t mac[6])
{
    for (size_t i = 0; i < s_count; i++) {
        if (memcmp(s_devices[i].mac, mac, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Evicts one device to make room for a new one: the oldest-last_seen device
 * that has no nickname and has never been persisted to NVS. Caller holds the
 * lock. Compacts the table by moving the last element into the freed slot.
 * Returns true when a victim was evicted.
 */
static bool evict_one_locked(void)
{
    int     victim = -1;
    int64_t oldest = 0;

    for (size_t i = 0; i < s_count; i++) {
        if (s_devices[i].nickname[0] != '\0' || s_persisted[i]) {
            continue;
        }
        if (victim < 0 || s_devices[i].last_seen < oldest) {
            victim = (int)i;
            oldest = s_devices[i].last_seen;
        }
    }
    if (victim < 0) {
        return false;
    }

    ESP_LOGW(TAG, "device table full, evicting %02x:%02x:%02x:%02x:%02x:%02x (last_seen=%lld)",
             s_devices[victim].mac[0], s_devices[victim].mac[1], s_devices[victim].mac[2],
             s_devices[victim].mac[3], s_devices[victim].mac[4], s_devices[victim].mac[5],
             (long long)s_devices[victim].last_seen);

    const size_t last = s_count - 1;
    if ((size_t)victim != last) {
        s_devices[victim]   = s_devices[last];
        s_seen[victim]      = s_seen[last];
        s_persisted[victim] = s_persisted[last];
        s_ports[victim]     = s_ports[last];   /* parallel array, move together */
    }
    memset(&s_ports[last], 0, sizeof(s_ports[last]));
    s_count--;
    return true;
}

/*
 * True when a name is just a machine identifier: a long unbroken run of hex
 * digits, such as the installation UUID Home Assistant publishes over mDNS.
 * Sixteen is comfortably longer than any real hostname that happens to be all
 * hex (think "beef" or "facade") and shorter than the 32-character UUIDs.
 */
static bool is_machine_id(const char *s)
{
    size_t n = 0;

    for (; s[n] != '\0'; n++) {
        if (!isxdigit((unsigned char)s[n])) {
            return false;
        }
    }
    return n >= 16;
}

/* Removes non-printable bytes, trims, strips a trailing ".local", clamps to
 * out_cap - 1 characters. Always NUL-terminates out. */
static void sanitize_hostname(const char *in, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }

    while (*in == ' ' || *in == '\t') {
        in++;
    }

    size_t len = strlen(in);
    static const char suffix[]  = ".local";
    const size_t       suf_len  = sizeof(suffix) - 1;
    if (len > suf_len && strcasecmp(in + (len - suf_len), suffix) == 0) {
        len -= suf_len;
    }

    size_t out_len = 0;
    for (size_t i = 0; i < len && out_len < out_cap - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 0x20 && c < 0x7f) {
            out[out_len++] = (char)c;
        }
    }
    while (out_len > 0 && (out[out_len - 1] == ' ' || out[out_len - 1] == '\t')) {
        out_len--;
    }
    out[out_len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* NVS persistence                                                           */
/* ------------------------------------------------------------------------- */

#define DEV_NVS_NS       "dev"
#define DEV_BLOB_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t  version;
    char     nickname[32];
    uint8_t  type_override;
    uint8_t  flags;
    int64_t  first_seen;
    uint32_t last_ip;
} dev_blob_t;

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

/* No lock held here on purpose - this does flash I/O. */
static esp_err_t nvs_write_user_fields(const netdash_device_t *d)
{
    dev_blob_t blob = {0};
    blob.version        = DEV_BLOB_VERSION;
    strncpy(blob.nickname, d->nickname, sizeof(blob.nickname) - 1);
    blob.type_override  = d->type_override;
    blob.flags          = d->flags;
    blob.first_seen     = d->first_seen;
    blob.last_ip        = d->ip;

    char key[13];
    mac_to_key(d->mac, key);

    nvs_handle_t h;
    esp_err_t    err = nvs_open(DEV_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", DEV_NVS_NS, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, key, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist device %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t nvs_erase_device(const uint8_t mac[6])
{
    char key[13];
    mac_to_key(mac, key);

    nvs_handle_t h;
    esp_err_t    err = nvs_open(DEV_NVS_NS, NVS_READWRITE, &h);
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

/* Marks idx as persisted after a successful write, re-finding it by MAC in
 * case the table changed shape while the lock was released for the NVS I/O. */
static void mark_persisted(const uint8_t mac[6])
{
    device_db_lock();
    int idx = find_index_locked(mac);
    if (idx >= 0) {
        s_persisted[idx] = true;
    }
    device_db_unlock();
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_events_lock;

esp_err_t device_db_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_events_lock == NULL) {
        s_events_lock = xSemaphoreCreateMutex();
        if (s_events_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t reg_err = esp_event_handler_register(NETDASH_EVENT, NETDASH_EVENT_TIME_SYNCED,
                                                     time_synced_handler, NULL);
    if (reg_err != ESP_OK) {
        ESP_LOGW(TAG, "could not subscribe to TIME_SYNCED: %s", esp_err_to_name(reg_err));
    }

    device_db_lock();
    s_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_seen, 0, sizeof(s_seen));
    memset(s_persisted, 0, sizeof(s_persisted));

    nvs_handle_t h;
    esp_err_t    err = nvs_open(DEV_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted devices yet");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", DEV_NVS_NS, esp_err_to_name(err));
    } else {
        nvs_iterator_t it   = NULL;
        esp_err_t      fres = nvs_entry_find_in_handle(h, NVS_TYPE_BLOB, &it);
        while (fres == ESP_OK && it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            uint8_t mac[6];
            if (s_count < NETDASH_MAX_DEVICES && key_to_mac(info.key, mac)) {
                dev_blob_t blob;
                size_t     sz = sizeof(blob);
                if (nvs_get_blob(h, info.key, &blob, &sz) == ESP_OK &&
                    sz == sizeof(blob) && blob.version == DEV_BLOB_VERSION) {
                    netdash_device_t *d = &s_devices[s_count];
                    memset(d, 0, sizeof(*d));
                    memcpy(d->mac, mac, 6);
                    d->ip = blob.last_ip;
                    strncpy(d->nickname, blob.nickname, sizeof(d->nickname) - 1);
                    d->type_override = blob.type_override;
                    d->flags         = blob.flags;
                    d->first_seen    = blob.first_seen;
                    d->last_seen     = 0;
                    d->rtt_ms        = -1;
                    d->miss_count    = 255; /* offline until seen this run */

                    const char *vendor = oui_lookup(mac);
                    if (vendor != NULL) {
                        strncpy(d->vendor, vendor, sizeof(d->vendor) - 1);
                    }
                    d->type = classify_device(d, 0);

                    s_persisted[s_count] = true;
                    s_seen[s_count]      = false;
                    memset(&s_ports[s_count], 0, sizeof(s_ports[s_count]));
                    ports_restore_locked((int)s_count);
                    s_count++;
                } else {
                    ESP_LOGW(TAG, "skipping unreadable blob for key %s", info.key);
                }
            }
            fres = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(h);
    }
    size_t loaded = s_count;
    device_db_unlock();

    ESP_LOGI(TAG, "loaded %u persisted device(s), capacity %u",
             (unsigned)loaded, (unsigned)NETDASH_MAX_DEVICES);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Port scan results                                                         */
/* ------------------------------------------------------------------------- */

#define PORTS_NVS_NS       "ports"
#define PORTS_BLOB_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  count;
    uint8_t  tier;
    int64_t  last_scan;
    uint16_t ports[NETDASH_MAX_OPEN_PORTS];
} ports_blob_t;

/*
 * Ports whose presence tells us something classify.c can use. Everything else
 * is recorded but does not feed classification.
 */
static uint16_t service_bit_for_port(uint16_t port)
{
    switch (port) {
    case 80:
    case 8080:
    case 8008:   return NETDASH_SVC_HTTP;
    case 443:
    case 8443:   return NETDASH_SVC_HTTPS;
    case 22:     return NETDASH_SVC_SSH;
    case 139:
    case 445:    return NETDASH_SVC_SMB;
    case 8123:   return NETDASH_SVC_HA;
    case 5353:   return NETDASH_SVC_SSDP;
    case 631:
    case 9100:   return NETDASH_SVC_PRINTER;
    case 8009:   return NETDASH_SVC_CAST;
    case 7000:   return NETDASH_SVC_AIRPLAY;
    default:     return 0;
    }
}

/* A small, deliberately incomplete table: the ports a home network explains. */
const char *netdash_port_service(uint16_t port)
{
    switch (port) {
    case 21:    return "ftp";
    case 22:    return "ssh";
    case 23:    return "telnet";
    case 25:    return "smtp";
    case 53:    return "dns";
    case 67:
    case 68:    return "dhcp";
    case 80:    return "http";
    case 110:   return "pop3";
    case 111:   return "rpcbind";
    case 123:   return "ntp";
    case 135:   return "msrpc";
    case 139:   return "netbios";
    case 143:   return "imap";
    case 161:   return "snmp";
    case 443:   return "https";
    case 445:   return "smb";
    case 515:   return "printer";
    case 548:   return "afp";
    case 554:   return "rtsp";
    case 631:   return "ipp";
    case 993:   return "imaps";
    case 995:   return "pop3s";
    case 1400:  return "sonos";
    case 1883:  return "mqtt";
    case 1900:  return "ssdp";
    case 2049:  return "nfs";
    case 3000:  return "grafana";
    case 3128:  return "squid";
    case 3306:  return "mysql";
    case 3389:  return "rdp";
    case 5000:  return "upnp";
    case 5001:  return "synology";
    case 5432:  return "postgres";
    case 5353:  return "mdns";
    case 5357:  return "wsd";
    case 5900:  return "vnc";
    case 6379:  return "redis";
    case 7000:  return "airplay";
    case 8006:  return "proxmox";
    case 8008:  return "cast-http";
    case 8009:  return "cast";
    case 8080:  return "http-alt";
    case 8096:  return "jellyfin";
    case 8123:  return "home-assistant";
    case 8443:  return "https-alt";
    case 8883:  return "mqtts";
    case 9000:  return "portainer";
    case 9090:  return "cockpit";
    case 9100:  return "jetdirect";
    case 32400: return "plex";
    case 51413: return "transmission";
    default:    return NULL;
    }
}

/*
 * Service ids are lowercase because they go out over JSON, but a dashboard
 * tile wants them written the way a person would. Most are handled by the
 * title-case fallback; this table is only for the ones that would come out
 * wrong, which is almost entirely acronyms.
 */
static const struct {
    const char *id;
    const char *label;
} s_service_labels[] = {
    { "http",           "HTTP" },
    { "https",          "HTTPS" },
    { "http-alt",       "HTTP" },
    { "https-alt",      "HTTPS" },
    { "ssh",            "SSH" },
    { "smb",            "SMB" },
    { "ftp",            "FTP" },
    { "dns",            "DNS" },
    { "dhcp",           "DHCP" },
    { "ntp",            "NTP" },
    { "smtp",           "SMTP" },
    { "imap",           "IMAP" },
    { "imaps",          "IMAPS" },
    { "pop3",           "POP3" },
    { "pop3s",          "POP3S" },
    { "snmp",           "SNMP" },
    { "rdp",            "RDP" },
    { "vnc",            "VNC" },
    { "ipp",            "IPP" },
    { "nfs",            "NFS" },
    { "afp",            "AFP" },
    { "mqtt",           "MQTT" },
    { "mqtts",          "MQTTS" },
    { "ssdp",           "SSDP" },
    { "mdns",           "mDNS" },
    { "rtsp",           "RTSP" },
    { "upnp",           "UPnP" },
    { "netbios",        "NetBIOS" },
    { "msrpc",          "MSRPC" },
    { "rpcbind",        "RPCbind" },
    { "wsd",            "WSD" },
    { "mysql",          "MySQL" },
    { "postgres",       "Postgres" },
    { "home-assistant", "Home Assistant" },
    { "cast",           "Cast" },
    { "cast-http",      "Cast" },
    { "airplay",        "AirPlay" },
    { "jetdirect",      "JetDirect" },
    { "proxmox",        "Proxmox" },
    { "synology",       "Synology" },
};

void netdash_service_label(const char *service, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (service == NULL || service[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < sizeof(s_service_labels) / sizeof(s_service_labels[0]); i++) {
        if (strcmp(service, s_service_labels[i].id) == 0) {
            strncpy(out, s_service_labels[i].label, cap - 1);
            out[cap - 1] = '\0';
            return;
        }
    }

    /* Fallback: "grafana" -> "Grafana", and a hyphen becomes a word break. */
    size_t o     = 0;
    bool   start = true;
    for (size_t i = 0; service[i] != '\0' && o + 1 < cap; i++) {
        char c = service[i];
        if (c == '-' || c == '_') {
            out[o++] = ' ';
            start    = true;
            continue;
        }
        out[o++] = start ? (char)toupper((unsigned char)c) : c;
        start    = false;
    }
    out[o] = '\0';
}

/* Caller holds the lock. */
static void ports_persist_locked(int idx)
{
    ports_blob_t blob = {
        .version   = PORTS_BLOB_VERSION,
        .count     = s_ports[idx].count,
        .tier      = s_ports[idx].tier,
        .last_scan = s_ports[idx].last_scan,
    };
    memcpy(blob.ports, s_ports[idx].ports, sizeof(blob.ports));

    uint8_t mac[6];
    memcpy(mac, s_devices[idx].mac, 6);

    char key[13];
    mac_to_key(mac, key);

    nvs_handle_t h;
    if (nvs_open(PORTS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, key, &blob, sizeof(blob)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Caller holds the lock. Restores a device's ports from NVS, if any. */
static void ports_restore_locked(int idx)
{
    char key[13];
    mac_to_key(s_devices[idx].mac, key);

    nvs_handle_t h;
    if (nvs_open(PORTS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    ports_blob_t blob;
    size_t       size = sizeof(blob);
    if (nvs_get_blob(h, key, &blob, &size) == ESP_OK && size == sizeof(blob) &&
        blob.version == PORTS_BLOB_VERSION) {
        s_ports[idx].count = blob.count > NETDASH_MAX_OPEN_PORTS
                                 ? NETDASH_MAX_OPEN_PORTS
                                 : blob.count;
        s_ports[idx].tier      = blob.tier > 3 ? 0 : blob.tier;
        s_ports[idx].last_scan = blob.last_scan;
        memcpy(s_ports[idx].ports, blob.ports, sizeof(s_ports[idx].ports));

        /* Re-apply the service bits the stored ports imply. */
        for (uint8_t i = 0; i < s_ports[idx].count; i++) {
            s_devices[idx].services |= service_bit_for_port(s_ports[idx].ports[i]);
        }
    }
    nvs_close(h);
}

bool device_db_get_ports(const uint8_t mac[6], netdash_ports_t *out)
{
    if (mac == NULL || out == NULL) {
        return false;
    }
    device_db_lock();
    const int idx = find_index_locked(mac);
    if (idx >= 0) {
        *out = s_ports[idx];
    }
    device_db_unlock();
    return idx >= 0;
}

bool device_db_add_open_port(const uint8_t mac[6], uint16_t port)
{
    if (mac == NULL || port == 0) {
        return false;
    }

    bool added = false;
    device_db_lock();
    const int idx = find_index_locked(mac);
    if (idx >= 0) {
        netdash_ports_t *p = &s_ports[idx];

        /* Insertion sort keeps the list ascending for the UI. */
        uint8_t at = 0;
        while (at < p->count && p->ports[at] < port) {
            at++;
        }
        if (at < p->count && p->ports[at] == port) {
            added = false;                      /* already known */
        } else if (p->count >= NETDASH_MAX_OPEN_PORTS) {
            added = false;                      /* list full, drop it */
        } else {
            memmove(&p->ports[at + 1], &p->ports[at],
                    (size_t)(p->count - at) * sizeof(p->ports[0]));
            p->ports[at] = port;
            p->count++;
            added = true;

            const uint16_t bit = service_bit_for_port(port);
            if (bit != 0 && (s_devices[idx].services & bit) == 0) {
                s_devices[idx].services |= bit;
                s_devices[idx].type = classify_device(&s_devices[idx], wifi_mgr_get_gateway());
            }
        }
    }
    device_db_unlock();
    return added;
}

void device_db_set_scan_progress(const uint8_t mac[6], uint8_t scanning_tier,
                                 uint32_t cursor, uint32_t tier_total)
{
    if (mac == NULL) {
        return;
    }
    device_db_lock();
    const int idx = find_index_locked(mac);
    if (idx >= 0) {
        s_ports[idx].scanning_tier = scanning_tier;
        s_ports[idx].cursor        = cursor;
        s_ports[idx].tier_total    = tier_total;
    }
    device_db_unlock();
}

void device_db_finish_tier(const uint8_t mac[6], uint8_t tier, int64_t now)
{
    if (mac == NULL) {
        return;
    }
    device_db_lock();
    const int idx = find_index_locked(mac);
    if (idx >= 0) {
        if (tier > s_ports[idx].tier) {
            s_ports[idx].tier = tier;
        }
        s_ports[idx].scanning_tier = 0;
        s_ports[idx].cursor        = 0;
        s_ports[idx].tier_total    = 0;
        s_ports[idx].last_scan     = now;
        ports_persist_locked(idx);
    }
    device_db_unlock();
}

void device_db_clear_ports(const uint8_t mac[6])
{
    if (mac == NULL) {
        return;
    }
    device_db_lock();
    const int idx = find_index_locked(mac);
    if (idx >= 0) {
        memset(&s_ports[idx], 0, sizeof(s_ports[idx]));

        char key[13];
        mac_to_key(mac, key);

        nvs_handle_t h;
        if (nvs_open(PORTS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_erase_key(h, key) == ESP_OK) {
                nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    device_db_unlock();
}

/* ------------------------------------------------------------------------- */
/* Read access                                                               */
/* ------------------------------------------------------------------------- */

size_t device_db_count(void)
{
    device_db_lock();
    size_t n = s_count;
    device_db_unlock();
    return n;
}

bool device_db_get_by_mac(const uint8_t mac[6], netdash_device_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (mac == NULL) {
        return false;
    }
    device_db_lock();
    int  idx   = find_index_locked(mac);
    bool found = idx >= 0;
    if (found && out != NULL) {
        *out = s_devices[idx];
    }
    device_db_unlock();
    return found;
}

bool device_db_get_at(size_t index, netdash_device_t *out)
{
    /* No internal locking: callers hold device_db_lock() across the whole
     * iteration (see device_db.h). */
    if (index >= s_count) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    if (out != NULL) {
        *out = s_devices[index];
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Scanner-facing mutation                                                   */
/* ------------------------------------------------------------------------- */

bool device_db_upsert_seen(const uint8_t mac[6], uint32_t ip, int16_t rtt_ms, int64_t now)
{
    if (mac == NULL) {
        return false;
    }

    netdash_device_t snapshot = {0};
    bool inserted           = false;
    bool became_online      = false;
    bool ip_changed         = false;
    bool persist_first_seen = false;
    bool table_full         = false;

    device_db_lock();

    int idx = find_index_locked(mac);
    if (idx < 0) {
        if (s_count >= NETDASH_MAX_DEVICES && !evict_one_locked()) {
            table_full = true;
        } else {
            idx                 = (int)s_count++;
            netdash_device_t *d = &s_devices[idx];
            memset(d, 0, sizeof(*d));
            memcpy(d->mac, mac, 6);
            d->rtt_ms = -1;
            memset(&s_ports[idx], 0, sizeof(s_ports[idx]));
            ports_restore_locked(idx);

            const char *vendor = oui_lookup(mac);
            if (vendor != NULL) {
                strncpy(d->vendor, vendor, sizeof(d->vendor) - 1);
            }
            s_seen[idx]      = false;
            s_persisted[idx] = false;
            inserted         = true;
        }
    }

    if (!table_full) {
        netdash_device_t *d = &s_devices[idx];

        became_online = d->miss_count > 0;
        ip_changed    = !inserted && d->ip != 0 && ip != 0 && d->ip != ip;

        if (d->first_seen == 0 && now != 0) {
            d->first_seen      = now;
            persist_first_seen = true;
        }

        d->ip         = ip;
        d->last_seen  = now;
        d->rtt_ms     = rtt_ms;
        d->miss_count = 0;
        s_seen[idx]   = true;

        d->type = classify_device(d, wifi_mgr_get_gateway());

        snapshot = *d;
    }

    device_db_unlock();

    if (table_full) {
        ESP_LOGW(TAG, "device table full (%u), refusing to add %02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned)NETDASH_MAX_DEVICES, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }

    if (persist_first_seen) {
        if (nvs_write_user_fields(&snapshot) == ESP_OK) {
            mark_persisted(snapshot.mac);
        }
    }

    char name[32];
    device_db_display_name(&snapshot, name, sizeof(name));

    if (inserted) {
        esp_event_post(NETDASH_EVENT, NETDASH_EVENT_DEVICE_NEW, &snapshot, sizeof(snapshot), 0);
        events_log_push(NETDASH_LOG_DEVICE_NEW, snapshot.mac, snapshot.ip, name);
    } else if (became_online) {
        esp_event_post(NETDASH_EVENT, NETDASH_EVENT_DEVICE_ONLINE, &snapshot, sizeof(snapshot), 0);
        events_log_push(NETDASH_LOG_DEVICE_ONLINE, snapshot.mac, snapshot.ip, name);
    }
    if (ip_changed) {
        esp_event_post(NETDASH_EVENT, NETDASH_EVENT_DEVICE_IP_CHANGED, &snapshot, sizeof(snapshot), 0);
        events_log_push(NETDASH_LOG_DEVICE_IP_CHANGED, snapshot.mac, snapshot.ip, name);
    }

    return inserted;
}

void device_db_mark_sweep_end(int64_t now)
{
    (void)now; /* last_seen is only ever advanced by device_db_upsert_seen(). */

    size_t offline_count = 0;

    device_db_lock();
    for (size_t i = 0; i < s_count; i++) {
        if (s_seen[i]) {
            s_seen[i] = false;
            continue;
        }
        netdash_device_t *d = &s_devices[i];
        if (d->miss_count < 255) {
            d->miss_count++;
        }
        if (d->miss_count == NETDASH_OFFLINE_AFTER_MISSES && offline_count < NETDASH_MAX_DEVICES) {
            memcpy(s_offline_macs[offline_count++], d->mac, 6);
        }
    }
    device_db_unlock();

    for (size_t i = 0; i < offline_count; i++) {
        netdash_device_t snap;
        if (!device_db_get_by_mac(s_offline_macs[i], &snap)) {
            continue; /* removed between the two passes */
        }
        char name[32];
        device_db_display_name(&snap, name, sizeof(name));
        esp_event_post(NETDASH_EVENT, NETDASH_EVENT_DEVICE_OFFLINE, &snap, sizeof(snap), 0);
        events_log_push(NETDASH_LOG_DEVICE_OFFLINE, snap.mac, snap.ip, name);
    }
}

/* ------------------------------------------------------------------------- */
/* Discovery-facing mutation                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t device_db_set_hostname(const uint8_t mac[6], const char *name, netdash_name_src_t src)
{
    if (mac == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char clean[32];
    sanitize_hostname(name, clean, sizeof(clean));
    if (clean[0] == '\0') {
        return ESP_OK; /* nothing usable in this name */
    }
    if (is_machine_id(clean)) {
        /*
         * A long run of hex is an installation UUID, not a name a person would
         * recognise. Home Assistant publishes exactly that as its mDNS name,
         * and taking it would beat the far friendlier name the router's DNS
         * gives for the same host. Drop it and let a lower-priority source win.
         */
        return ESP_OK;
    }

    device_db_lock();
    int idx = find_index_locked(mac);
    if (idx < 0) {
        device_db_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    netdash_device_t *d = &s_devices[idx];
    d->sources |= NETDASH_SRC_BIT(src); /* every source that ever named it */

    if ((uint8_t)src >= d->name_src) {
        strncpy(d->hostname, clean, sizeof(d->hostname) - 1);
        d->hostname[sizeof(d->hostname) - 1] = '\0';
        d->name_src                          = (uint8_t)src;
        d->type = classify_device(d, wifi_mgr_get_gateway());
    }
    device_db_unlock();
    return ESP_OK;
}

esp_err_t device_db_set_services(const uint8_t mac[6], uint16_t bits)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_db_lock();
    int idx = find_index_locked(mac);
    if (idx < 0) {
        device_db_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    netdash_device_t *d      = &s_devices[idx];
    const uint16_t     before = d->services;
    d->services |= bits;
    if (d->services != before) {
        d->type = classify_device(d, wifi_mgr_get_gateway());
    }
    device_db_unlock();
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* User-facing mutation (persisted)                                         */
/* ------------------------------------------------------------------------- */

esp_err_t device_db_set_user(const uint8_t mac[6], const char *nickname,
                             int type_override, int flags)
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    netdash_device_t snapshot;
    device_db_lock();
    int idx = find_index_locked(mac);
    if (idx < 0) {
        device_db_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    netdash_device_t *d = &s_devices[idx];
    if (nickname != NULL) {
        strncpy(d->nickname, nickname, sizeof(d->nickname) - 1);
        d->nickname[sizeof(d->nickname) - 1] = '\0';
    }
    if (type_override >= 0 && type_override < NETDASH_TYPE_MAX) {
        d->type_override = (uint8_t)type_override;
    }
    if (flags >= 0 && flags <= 0xff) {
        d->flags = (uint8_t)flags;
    }
    snapshot = *d;
    device_db_unlock();

    esp_err_t err = nvs_write_user_fields(&snapshot);
    if (err == ESP_OK) {
        mark_persisted(snapshot.mac);
    }
    return err;
}

esp_err_t device_db_remove(const uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_db_lock();
    int  idx   = find_index_locked(mac);
    bool found = idx >= 0;
    if (found) {
        const size_t last = s_count - 1;
        if ((size_t)idx != last) {
            s_devices[idx]   = s_devices[last];
            s_seen[idx]      = s_seen[last];
            s_persisted[idx] = s_persisted[last];
        }
        s_count--;
    }
    device_db_unlock();

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    return nvs_erase_device(mac);
}

esp_err_t device_db_ensure(const uint8_t mac[6])
{
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_db_lock();
    bool no_room = false;
    int  idx     = find_index_locked(mac);
    if (idx < 0) {
        if (s_count >= NETDASH_MAX_DEVICES && !evict_one_locked()) {
            no_room = true;
        } else {
            idx                 = (int)s_count++;
            netdash_device_t *d = &s_devices[idx];
            memset(d, 0, sizeof(*d));
            memcpy(d->mac, mac, 6);
            d->rtt_ms     = -1;
            d->miss_count = 255; /* offline: it has not actually been seen */
            memset(&s_ports[idx], 0, sizeof(s_ports[idx]));
            ports_restore_locked(idx);

            const char *vendor = oui_lookup(mac);
            if (vendor != NULL) {
                strncpy(d->vendor, vendor, sizeof(d->vendor) - 1);
            }
            d->type = classify_device(d, 0);

            s_seen[idx]      = false;
            s_persisted[idx] = false;
        }
    }
    device_db_unlock();

    if (no_room) {
        ESP_LOGW(TAG, "device table full, cannot import %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void device_db_display_name(const netdash_device_t *dev, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (dev == NULL) {
        buf[0] = '\0';
        return;
    }

    if (dev->nickname[0] != '\0') {
        snprintf(buf, len, "%s", dev->nickname);
    } else if (dev->hostname[0] != '\0') {
        snprintf(buf, len, "%s", dev->hostname);
    } else if (dev->vendor[0] != '\0') {
        snprintf(buf, len, "%s %02x%02x", dev->vendor, dev->mac[4], dev->mac[5]);
    } else {
        snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 dev->mac[0], dev->mac[1], dev->mac[2],
                 dev->mac[3], dev->mac[4], dev->mac[5]);
    }
}

/* ------------------------------------------------------------------------- */
/* Events ring buffer                                                        */
/* ------------------------------------------------------------------------- */

static netdash_event_rec_t s_events[NETDASH_EVENTS_LOG_SIZE];
static size_t              s_events_head;  /* next slot to write            */
static size_t              s_events_count; /* valid entries, <= LOG_SIZE    */

void events_log_push(netdash_log_type_t type, const uint8_t mac[6], uint32_t ip, const char *text)
{
    if (s_events_lock == NULL) {
        /* device_db_init() has not run yet; nothing sensible to do. */
        ESP_LOGD(TAG, "event %s dropped (log not initialised): %s",
                 netdash_log_type_name(type), text ? text : "");
        return;
    }

    xSemaphoreTake(s_events_lock, portMAX_DELAY);

    netdash_event_rec_t *e = &s_events[s_events_head];
    memset(e, 0, sizeof(*e));
    e->ts   = now_or_zero();
    e->type = (uint8_t)type;
    if (mac != NULL) {
        memcpy(e->mac, mac, 6);
    }
    e->ip = ip;
    if (text != NULL) {
        strncpy(e->text, text, sizeof(e->text) - 1);
    }

    s_events_head = (s_events_head + 1) % NETDASH_EVENTS_LOG_SIZE;
    if (s_events_count < NETDASH_EVENTS_LOG_SIZE) {
        s_events_count++;
    }

    xSemaphoreGive(s_events_lock);
}

bool events_log_get(size_t index, netdash_event_rec_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (s_events_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_events_lock, portMAX_DELAY);
    bool found = index < s_events_count;
    if (found) {
        /* index 0 = newest = the slot written just before s_events_head. */
        size_t pos = (s_events_head + NETDASH_EVENTS_LOG_SIZE - 1 - index) % NETDASH_EVENTS_LOG_SIZE;
        if (out != NULL) {
            *out = s_events[pos];
        }
    }
    xSemaphoreGive(s_events_lock);
    return found;
}

size_t events_log_count(void)
{
    if (s_events_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_events_lock, portMAX_DELAY);
    size_t n = s_events_count;
    xSemaphoreGive(s_events_lock);
    return n;
}

const char *netdash_log_type_name(netdash_log_type_t type)
{
    switch (type) {
    case NETDASH_LOG_INFO:              return "info";
    case NETDASH_LOG_DEVICE_NEW:        return "device_new";
    case NETDASH_LOG_DEVICE_ONLINE:     return "device_online";
    case NETDASH_LOG_DEVICE_OFFLINE:    return "device_offline";
    case NETDASH_LOG_DEVICE_IP_CHANGED: return "device_ip_changed";
    case NETDASH_LOG_SCAN:              return "scan";
    case NETDASH_LOG_WIFI:              return "wifi";
    default:                            return "info";
    }
}

/* ------------------------------------------------------------------------- */
/* Self-test (WP8 integration only)                                          */
/* ------------------------------------------------------------------------- */

#ifdef NETDASH_SELFTEST

static bool s_selftest_ok;

#define ST_CHECK(cond, ...)                                                   \
    do {                                                                      \
        if (cond) {                                                           \
            ESP_LOGI(TAG, "  PASS: " __VA_ARGS__);                            \
        } else {                                                              \
            ESP_LOGE(TAG, "  FAIL: " __VA_ARGS__);                            \
            s_selftest_ok = false;                                            \
        }                                                                     \
    } while (0)

static void selftest_key_roundtrip(void)
{
    const uint8_t mac[6] = {0xaa, 0xbb, 0xcc, 0x01, 0x02, 0x03};
    char          key[13];
    uint8_t       back[6] = {0};

    mac_to_key(mac, key);
    ST_CHECK(strcmp(key, "aabbcc010203") == 0, "mac_to_key produces lowercase hex (%s)", key);
    ST_CHECK(key_to_mac(key, back) && memcmp(mac, back, 6) == 0, "key_to_mac round-trips");
    ST_CHECK(!key_to_mac("short", back), "key_to_mac rejects a short key");
    ST_CHECK(!key_to_mac("zzbbcc010203", back), "key_to_mac rejects non-hex");
}

static void selftest_hostname_sanitise(void)
{
    char out[32];

    sanitize_hostname("  My-PC.local  ", out, sizeof(out));
    ST_CHECK(strcmp(out, "My-PC") == 0, "sanitize_hostname strips '.local' and trims (%s)", out);

    sanitize_hostname("bad\x01name", out, sizeof(out));
    ST_CHECK(strcmp(out, "badname") == 0, "sanitize_hostname drops control bytes (%s)", out);

    sanitize_hostname(NULL, out, sizeof(out));
    ST_CHECK(out[0] == '\0', "sanitize_hostname handles NULL");
}

static void selftest_display_name(void)
{
    netdash_device_t d = {0};
    memcpy(d.mac, (uint8_t[]){0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, 6);
    char buf[48];

    device_db_display_name(&d, buf, sizeof(buf));
    ST_CHECK(strcmp(buf, "00:11:22:33:44:55") == 0, "display_name falls back to the MAC (%s)", buf);

    strcpy(d.vendor, "Raspberry Pi");
    device_db_display_name(&d, buf, sizeof(buf));
    ST_CHECK(strcmp(buf, "Raspberry Pi 4455") == 0, "display_name falls back to vendor+bytes (%s)", buf);

    strcpy(d.hostname, "kitchenpi");
    device_db_display_name(&d, buf, sizeof(buf));
    ST_CHECK(strcmp(buf, "kitchenpi") == 0, "display_name prefers hostname over vendor (%s)", buf);

    strcpy(d.nickname, "Kitchen Pi");
    device_db_display_name(&d, buf, sizeof(buf));
    ST_CHECK(strcmp(buf, "Kitchen Pi") == 0, "display_name prefers nickname over hostname (%s)", buf);
}

static void selftest_classify(void)
{
    netdash_device_t d;

    memset(&d, 0, sizeof(d));
    d.ip = 0xc0a80101; /* 192.168.1.1 */
    ST_CHECK(classify_device(&d, 0xc0a80101) == NETDASH_TYPE_ROUTER, "classify: gateway IP -> router");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "ASUSTek");
    ST_CHECK(classify_device(&d, 0xc0a80101) == NETDASH_TYPE_MESH_NODE, "classify: ASUS, not gateway -> mesh_node");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "Netgear");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_SWITCH, "classify: Netgear, no services -> switch");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "Synology");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_NAS, "classify: Synology vendor -> nas");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "LG Electronics");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_TV, "classify: LG Electronics -> tv");

    memset(&d, 0, sizeof(d));
    d.services = NETDASH_SVC_HA;
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_HUB, "classify: HA service -> hub");

    memset(&d, 0, sizeof(d));
    d.services = NETDASH_SVC_WORKSTATION;
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_PC, "classify: workstation service -> pc");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "Espressif");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_IOT, "classify: Espressif -> iot");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "HP");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_PRINTER, "classify: HP vendor -> printer");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "Apple");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_PHONE, "classify: Apple, no services -> phone");

    memset(&d, 0, sizeof(d));
    strcpy(d.vendor, "Realtek");
    ST_CHECK(classify_device(&d, 0) == NETDASH_TYPE_UNKNOWN, "classify: nothing matches -> unknown");

    for (int t = 0; t < NETDASH_TYPE_MAX; t++) {
        const char *name = netdash_type_name((netdash_type_t)t);
        ST_CHECK(netdash_type_from_name(name) == (netdash_type_t)t,
                 "type name round-trip for '%s'", name);
    }
}

static void selftest_events_ring(void)
{
    for (int i = 0; i < NETDASH_EVENTS_LOG_SIZE + 5; i++) {
        char text[48];
        snprintf(text, sizeof(text), "evt-%d", i);
        events_log_push(NETDASH_LOG_INFO, NULL, 0, text);
    }
    ST_CHECK(events_log_count() == NETDASH_EVENTS_LOG_SIZE,
             "events ring caps at %d entries", NETDASH_EVENTS_LOG_SIZE);

    netdash_event_rec_t rec;
    ST_CHECK(events_log_get(0, &rec) && strcmp(rec.text, "evt-104") == 0,
             "events_log_get(0) is the newest entry (%s)", rec.text);
    ST_CHECK(!events_log_get(NETDASH_EVENTS_LOG_SIZE, &rec), "events_log_get() bounds-checks");
}

static void selftest_sweep_lifecycle(void)
{
    const uint8_t mac[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}; /* locally administered: no OUI */

    device_db_remove(mac); /* clean slate, ignore ESP_ERR_NOT_FOUND */

    bool inserted = device_db_upsert_seen(mac, 0x0a000001u, 4, 0);
    ST_CHECK(inserted, "upsert_seen inserts an unknown device");

    netdash_device_t snap;
    ST_CHECK(device_db_get_by_mac(mac, &snap) && snap.first_seen == 0,
             "first_seen stays 0 while now==0 (no NTP yet)");

    device_db_upsert_seen(mac, 0x0a000001u, 4, 1700000000);
    device_db_get_by_mac(mac, &snap);
    ST_CHECK(snap.first_seen == 1700000000, "first_seen backfills once a real time arrives");

    for (int i = 0; i < NETDASH_OFFLINE_AFTER_MISSES; i++) {
        device_db_mark_sweep_end(0);
    }
    device_db_get_by_mac(mac, &snap);
    ST_CHECK(snap.miss_count == NETDASH_OFFLINE_AFTER_MISSES && !netdash_device_online(&snap),
             "miss_count reaches the offline threshold after %d empty sweeps",
             NETDASH_OFFLINE_AFTER_MISSES);

    bool reinserted = device_db_upsert_seen(mac, 0x0a000001u, 4, 1700000100);
    ST_CHECK(!reinserted, "a known device is never reported as newly inserted");
    device_db_get_by_mac(mac, &snap);
    ST_CHECK(snap.miss_count == 0, "upsert_seen resets miss_count");

    device_db_remove(mac);
}

static void selftest_eviction(void)
{
    device_db_lock();
    size_t before = s_count;
    device_db_unlock();

    /*
     * Fill every remaining slot with disposable (no nickname, never
     * persisted) devices. now==0 keeps first_seen at 0 so upsert_seen does
     * not backfill-and-persist them, which would make them ineligible for
     * eviction and defeat this test.
     */
    for (size_t i = before; i < NETDASH_MAX_DEVICES; i++) {
        uint8_t mac[6] = {0x02, 0xf0, (uint8_t)(i >> 8), (uint8_t)i, 0x00, 0x01};
        device_db_upsert_seen(mac, 0, -1, 0);
    }
    ST_CHECK(device_db_count() == NETDASH_MAX_DEVICES, "table fills to capacity (%u)",
             (unsigned)NETDASH_MAX_DEVICES);

    uint8_t newcomer[6] = {0x02, 0xf1, 0x00, 0x00, 0x00, 0x01};
    bool    inserted    = device_db_upsert_seen(newcomer, 0, -1, 0);
    ST_CHECK(inserted, "a new device evicts the oldest disposable entry instead of being refused");
    ST_CHECK(device_db_count() == NETDASH_MAX_DEVICES, "table stays at capacity after eviction");

    /* Clean up every synthetic device this test added. */
    device_db_remove(newcomer);
    for (size_t i = before; i < NETDASH_MAX_DEVICES; i++) {
        uint8_t mac[6] = {0x02, 0xf0, (uint8_t)(i >> 8), (uint8_t)i, 0x00, 0x01};
        device_db_remove(mac);
    }
}

bool device_db_selftest(void)
{
    s_selftest_ok = true;
    ESP_LOGI(TAG, "device_db_selftest: starting");

    selftest_key_roundtrip();
    selftest_hostname_sanitise();
    selftest_display_name();
    selftest_classify();
    selftest_events_ring();
    selftest_sweep_lifecycle();
    selftest_eviction();

    ESP_LOGI(TAG, "device_db_selftest: %s", s_selftest_ok ? "ALL PASS" : "FAILED");
    return s_selftest_ok;
}

#endif /* NETDASH_SELFTEST */
