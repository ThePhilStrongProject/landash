/*
 * NetDash device database.
 *
 * A fixed-size RAM table of every host seen on the LAN, plus the user-owned
 * fields (nickname / type override / hidden flag) persisted to NVS namespace
 * "dev" under the 12-hex-lowercase MAC.
 *
 * Threading: the table is shared between the scanner, the discovery tasks, the
 * HTTP server and the UI. Every accessor except the iteration helper takes the
 * lock internally. Callers that iterate with device_db_get_at() must hold the
 * lock across the whole loop (see device_db_lock()).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETDASH_MAX_DEVICES CONFIG_NETDASH_MAX_DEVICES

/* Consecutive missed sweeps before a device is reported offline. */
#define NETDASH_OFFLINE_AFTER_MISSES 3

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum {
    NETDASH_TYPE_UNKNOWN = 0,
    NETDASH_TYPE_ROUTER,
    NETDASH_TYPE_MESH_NODE,
    NETDASH_TYPE_SWITCH,
    NETDASH_TYPE_NAS,
    NETDASH_TYPE_TV,
    NETDASH_TYPE_HUB,
    NETDASH_TYPE_PHONE,
    NETDASH_TYPE_PC,
    NETDASH_TYPE_IOT,
    NETDASH_TYPE_PRINTER,
    NETDASH_TYPE_CAST,
    NETDASH_TYPE_MAX
} netdash_type_t;

/* Advertised or probed services; netdash_device_t::services bitmask. */
#define NETDASH_SVC_HTTP        (1u << 0)
#define NETDASH_SVC_HTTPS       (1u << 1)
#define NETDASH_SVC_SSH         (1u << 2)
#define NETDASH_SVC_SMB         (1u << 3)
#define NETDASH_SVC_HAP         (1u << 4)   /* HomeKit accessory protocol  */
#define NETDASH_SVC_HA          (1u << 5)   /* Home Assistant / ESPHome    */
#define NETDASH_SVC_CAST        (1u << 6)   /* Google Cast                 */
#define NETDASH_SVC_AIRPLAY     (1u << 7)
#define NETDASH_SVC_PRINTER     (1u << 8)   /* IPP / LPD                   */
#define NETDASH_SVC_SSDP        (1u << 9)
#define NETDASH_SVC_WORKSTATION (1u << 10)
#define NETDASH_SVC_COUNT       11

/* netdash_device_t::flags */
#define NETDASH_FLAG_HIDDEN     (1u << 0)

/*
 * Hostname sources, ordered by priority: a name from a higher-numbered source
 * replaces a name from a lower-numbered one; a name from an equal or higher
 * source refreshes the stored name.
 */
typedef enum {
    NETDASH_NAME_SRC_NONE = 0,
    NETDASH_NAME_SRC_SSDP = 1,
    NETDASH_NAME_SRC_NBNS = 2,
    NETDASH_NAME_SRC_RDNS = 3,
    NETDASH_NAME_SRC_MDNS = 4,
} netdash_name_src_t;

/* netdash_device_t::sources: every source that has ever named this device. */
#define NETDASH_SRC_BIT(src) ((uint8_t)(1u << (src)))

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;                    /* last known IPv4, host byte order      */
    char     hostname[32];          /* best auto name, see name_src          */
    char     vendor[24];            /* OUI lookup, empty when unknown        */
    uint8_t  type;                  /* netdash_type_t, auto-classified       */
    uint16_t services;              /* NETDASH_SVC_ bitmask                  */
    int64_t  first_seen;            /* unix seconds, 0 before NTP sync       */
    int64_t  last_seen;             /* unix seconds, 0 before NTP sync       */
    int16_t  rtt_ms;                /* -1 when ARP-only (no ICMP reply)      */
    uint8_t  miss_count;            /* consecutive missed sweeps             */
    /* Name provenance; needed to implement the documented source priority.  */
    uint8_t  name_src;              /* netdash_name_src_t of hostname        */
    uint8_t  sources;               /* NETDASH_SRC_BIT mask of all sources   */
    /* User fields, persisted to NVS.                                        */
    char     nickname[32];
    uint8_t  type_override;         /* 0 = auto, else netdash_type_t         */
    uint8_t  flags;                 /* NETDASH_FLAG_                         */
} netdash_device_t;

static inline bool netdash_device_online(const netdash_device_t *d)
{
    return d != NULL && d->miss_count == 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* Creates the lock and reloads persisted user fields from NVS. */
esp_err_t device_db_init(void);

/* ------------------------------------------------------------------------- */
/* Read access                                                               */
/* ------------------------------------------------------------------------- */

size_t device_db_count(void);

/* Copies the record for mac into out. Returns false when not present. */
bool device_db_get_by_mac(const uint8_t mac[6], netdash_device_t *out);

/*
 * Copies the record at index (0 .. device_db_count() - 1) into out.
 * Indices are only stable while the lock is held, so hold device_db_lock()
 * across the whole iteration.
 */
bool device_db_get_at(size_t index, netdash_device_t *out);

/* ------------------------------------------------------------------------- */
/* Scanner-facing mutation                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Records that mac answered at ip at time now (unix seconds, may be 0).
 * Inserts the device when unknown, resets miss_count, and posts NETDASH_EVENT
 * DEVICE_NEW / DEVICE_ONLINE / DEVICE_IP_CHANGED as appropriate.
 *
 * rtt_ms is the ICMP round trip, or -1 when the host was only found via ARP.
 * Returns true when the device was newly inserted.
 */
bool device_db_upsert_seen(const uint8_t mac[6], uint32_t ip, int16_t rtt_ms, int64_t now);

/*
 * Called once at the end of every sweep. Increments miss_count of every device
 * not seen during the sweep and posts DEVICE_OFFLINE for those reaching
 * NETDASH_OFFLINE_AFTER_MISSES.
 */
void device_db_mark_sweep_end(int64_t now);

/* ------------------------------------------------------------------------- */
/* Discovery-facing mutation                                                 */
/* ------------------------------------------------------------------------- */

/* Applies name when src outranks the source of the stored name. */
esp_err_t device_db_set_hostname(const uint8_t mac[6], const char *name, netdash_name_src_t src);

/* ORs bits into the service mask of the device. */
esp_err_t device_db_set_services(const uint8_t mac[6], uint16_t bits);

/* ------------------------------------------------------------------------- */
/* User-facing mutation (persisted)                                          */
/* ------------------------------------------------------------------------- */

/*
 * Updates the user fields and writes them to NVS.
 * nickname NULL leaves the nickname unchanged, an empty string clears it.
 * type_override and flags accept -1 to leave the field unchanged.
 */
esp_err_t device_db_set_user(const uint8_t mac[6], const char *nickname,
                             int type_override, int flags);

/* Forgets a device, in RAM and in NVS. */
esp_err_t device_db_remove(const uint8_t mac[6]);

/*
 * Creates an offline, unnamed record for mac if one does not already exist.
 * Does not post DEVICE_NEW or touch the events log - this is for
 * POST /api/devices/import, which must be able to create an entry for a
 * device that is not currently on the network so its nickname can be
 * restored via device_db_set_user() right after. A no-op (ESP_OK) when the
 * device is already known.
 */
esp_err_t device_db_ensure(const uint8_t mac[6]);

/* nickname, else hostname, else vendor plus last 2 MAC bytes, else the MAC. */
void device_db_display_name(const netdash_device_t *dev, char *buf, size_t len);

/* ------------------------------------------------------------------------- */
/* Locking                                                                   */
/* ------------------------------------------------------------------------- */

void device_db_lock(void);
void device_db_unlock(void);

/* ------------------------------------------------------------------------- */
/* Events ring buffer (what GET /api/events serves)                          */
/* ------------------------------------------------------------------------- */

#define NETDASH_EVENTS_LOG_SIZE 100

typedef enum {
    NETDASH_LOG_INFO = 0,
    NETDASH_LOG_DEVICE_NEW,
    NETDASH_LOG_DEVICE_ONLINE,
    NETDASH_LOG_DEVICE_OFFLINE,
    NETDASH_LOG_DEVICE_IP_CHANGED,
    NETDASH_LOG_SCAN,
    NETDASH_LOG_WIFI,
} netdash_log_type_t;

typedef struct {
    int64_t  ts;            /* unix seconds, 0 before NTP sync  */
    uint8_t  type;          /* netdash_log_type_t               */
    uint8_t  mac[6];        /* all zero when not device scoped  */
    uint32_t ip;            /* 0 when not applicable            */
    char     text[48];
} netdash_event_rec_t;

/* mac may be NULL, text may be NULL. Safe to call from any task. */
void events_log_push(netdash_log_type_t type, const uint8_t mac[6], uint32_t ip, const char *text);

/* index 0 is the newest entry. Returns false past the end. */
bool events_log_get(size_t index, netdash_event_rec_t *out);

size_t events_log_count(void);

/* Stable lowercase name used by the REST API, e.g. "device_new". */
const char *netdash_log_type_name(netdash_log_type_t type);

/* ------------------------------------------------------------------------- */
/* Self-test (WP8 integration only)                                          */
/* ------------------------------------------------------------------------- */

#ifdef NETDASH_SELFTEST
/*
 * Exercises device_db's internal logic (key encoding, hostname sanitising,
 * classification wiring, the events ring buffer, sweep/eviction bookkeeping)
 * and logs a PASS/FAIL line per case at INFO/ERROR. Requires device_db_init()
 * to have already run. Mutates the live device table (inserts synthetic
 * devices), so only call it from a dedicated diagnostic build, right after
 * device_db_init() and before any other module touches the table. Returns
 * true when every case passed. Only declared/compiled when NETDASH_SELFTEST
 * is defined, e.g. by adding
 * `target_compile_definitions(${COMPONENT_LIB} PRIVATE NETDASH_SELFTEST)`
 * to main/CMakeLists.txt for a one-off diagnostic build.
 */
bool device_db_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
