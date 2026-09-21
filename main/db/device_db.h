/*
 * LANDA.SH device database.
 *
 * Two tiers. A fixed-size table in RAM holds the active devices - online, or
 * seen recently - which is everything the scanner, discovery and the polled
 * device list work on. Every device ever seen also has a record in the
 * register in flash (db/dev_store.h), which keeps its nickname, type, flags,
 * first and last seen, name and open ports. When the RAM table fills, the
 * device offline longest is dropped from RAM, its record kept; when it is seen
 * again it is loaded back. Up to CONFIG_NETDASH_MAX_DEVICES active and
 * CONFIG_NETDASH_REGISTER_DEVICES remembered.
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
    /* Appended, never reordered: the value is persisted as type_override. */
    NETDASH_TYPE_CONSOLE,
    NETDASH_TYPE_PRINTER_3D,
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
 * This hardware MAC answers at more than one address - a Wi-Fi extender in
 * client mode rewrites every wired device behind it to its own MAC - so the
 * entry is pinned to its IP rather than following the MAC. Set by device_db,
 * never by the user; see "Devices that share a MAC" in docs/API.md.
 */
#define NETDASH_FLAG_SHARED_MAC (1u << 1)
/* The bits device_db_set_user() may change. The rest belong to device_db. */
#define NETDASH_FLAGS_USER      NETDASH_FLAG_HIDDEN

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
    /*
     * The key. The hardware MAC for every ordinary device; for a device behind
     * a shared MAC (NETDASH_FLAG_SHARED_MAC) other than the one that holds the
     * MAC itself, a synthetic id starting 0x03 - multicast, so it can never
     * collide with a real device - derived from hw_mac and ip.
     */
    uint8_t  mac[6];
    uint8_t  hw_mac[6];             /* the MAC it answers ARP with           */
    uint32_t ip;                    /* last known IPv4, host byte order      */
    char     hostname[32];          /* best auto name, see name_src          */
    const char *vendor;             /* OUI name, "" when unknown; never NULL.
                                       Points into the OUI table in flash, so
                                       it costs 4 bytes, not a copy.         */
    uint8_t  type;                  /* netdash_type_t, auto-classified       */
    uint16_t services;              /* NETDASH_SVC_ bitmask                  */
    int64_t  first_seen;            /* unix seconds, 0 before NTP sync       */
    int64_t  last_seen;             /* unix seconds, 0 before NTP sync       */
    int16_t  rtt_ms;                /* -1 when ARP-only (no ICMP reply)      */
    uint8_t  miss_count;            /* consecutive missed sweeps             */
    /* Name provenance; needed to implement the documented source priority.  */
    uint8_t  name_src;              /* netdash_name_src_t of hostname        */
    uint8_t  sources;               /* NETDASH_SRC_BIT mask of all sources   */
    /* User fields, kept in the register.                                    */
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

/*
 * Opens the register (after icons_init(), which mounts its partition), moves
 * any devices still in the older NVS storage into it on the first boot that
 * has one, and loads the most recently seen devices into RAM.
 */
esp_err_t device_db_init(void);

/* ------------------------------------------------------------------------- */
/* Read access                                                               */
/* ------------------------------------------------------------------------- */

/* Devices in the RAM table - the bound for device_db_get_at(). */
size_t device_db_count(void);

/* Every device known: those in the register, or in RAM when it is unavailable. */
size_t device_db_known_count(void);

/*
 * Copies the device into out: from RAM when it is active, otherwise from its
 * record in the register, as an offline device (miss_count 255). False when
 * the device is unknown. May read flash, so never call it holding the lock.
 */
bool device_db_get_by_mac(const uint8_t mac[6], netdash_device_t *out);

/*
 * Iterating the devices that are only in the register, for the "older
 * devices" list and the export. slot runs 0 .. device_db_register_slots() - 1;
 * false for a free slot or a device that is active in RAM. Reads flash.
 */
size_t device_db_register_slots(void);
bool   device_db_get_archived(size_t slot, netdash_device_t *out);

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
 * mac is the hardware MAC as ARP reported it. When one MAC turns out to answer
 * at several addresses at once, this routes each address to its own entry
 * rather than moving a single entry back and forth; callers do not need to
 * know, but must pass every (mac, ip) pair they see, not one per MAC.
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
 * Updates the user fields and writes them to the register, whether the
 * device is active or only remembered.
 * nickname NULL leaves the nickname unchanged, an empty string clears it.
 * type_override and flags accept -1 to leave the field unchanged.
 */
esp_err_t device_db_set_user(const uint8_t mac[6], const char *nickname,
                             int type_override, int flags);

/* Forgets a device: from RAM, from the register, and its notes. */
esp_err_t device_db_remove(const uint8_t mac[6]);

/*
 * Creates an offline, unnamed record for mac in the register if the device is
 * not already known. Does not post DEVICE_NEW or touch the events log - this is for
 * POST /api/devices/import, which must be able to create an entry for a
 * device that is not currently on the network so its nickname can be
 * restored via device_db_set_user() right after. A no-op (ESP_OK) when the
 * device is already known.
 */
esp_err_t device_db_ensure(const uint8_t mac[6]);

/*
 * Recreates an entry for a device behind a shared MAC from an export: id is
 * its key, hw_mac and ip what it answers with. Like device_db_ensure(), a
 * no-op when id is already known, and posts nothing.
 */
esp_err_t device_db_ensure_shared(const uint8_t id[6], const uint8_t hw_mac[6], uint32_t ip);

/*
 * nickname, else hostname, else vendor plus last 2 MAC bytes, else the MAC.
 * An unnamed entry behind a shared MAC also gets its address's last octet,
 * since the MAC alone would name two entries identically.
 */
void device_db_display_name(const netdash_device_t *dev, char *buf, size_t len);

/* ------------------------------------------------------------------------- */
/* Port scan results                                                         */
/* ------------------------------------------------------------------------- */

#define NETDASH_MAX_OPEN_PORTS CONFIG_NETDASH_PORTSCAN_MAX_OPEN

/*
 * Port data lives outside netdash_device_t, because that struct is copied by
 * value into every event and out of every read. The list itself is kept only
 * in the device's record in flash; RAM holds the count, the tier and the scan
 * in progress.
 */
typedef struct {
    uint16_t ports[NETDASH_MAX_OPEN_PORTS]; /* open TCP ports, ascending    */
    uint8_t  count;
    uint8_t  tier;          /* highest tier finished, 0 = none yet          */
    uint8_t  scanning_tier; /* tier in progress, 0 = idle                   */
    uint32_t cursor;        /* next index within the tier in progress       */
    uint32_t tier_total;    /* probes in the tier in progress, 0 = idle     */
    int64_t  last_scan;     /* unix seconds a tier last finished, 0 = never */
} netdash_ports_t;

/*
 * The whole port record for mac, including the list, which is read from
 * flash: for a single device's view. False when mac is unknown. Never call it
 * holding the lock.
 */
bool device_db_get_ports(const uint8_t mac[6], netdash_ports_t *out);

/*
 * The same without the list (ports[] zeroed, count still right), and without
 * touching flash for an active device: for anything that runs often, like the
 * polled device list and the port scanner.
 */
bool device_db_get_port_summary(const uint8_t mac[6], netdash_ports_t *out);

/*
 * Records an open TCP port. Ignores duplicates, keeps the list ascending and
 * drops anything past NETDASH_MAX_OPEN_PORTS. Also ORs in the service bit the
 * port implies (445 means SMB and so on), which lets classify.c identify
 * devices that advertise nothing over mDNS or SSDP.
 * Returns true when the port was newly recorded.
 */
bool device_db_add_open_port(const uint8_t mac[6], uint16_t port);

/* Updates the progress fields shown by the API. */
void device_db_set_scan_progress(const uint8_t mac[6], uint8_t scanning_tier,
                                 uint32_t cursor, uint32_t tier_total);

/*
 * Marks a tier finished for mac and persists the whole port record.
 * Clears the in-progress fields.
 */
void device_db_finish_tier(const uint8_t mac[6], uint8_t tier, int64_t now);

/* Forgets every port result for mac, in RAM and in the register (before a rescan). */
void device_db_clear_ports(const uint8_t mac[6]);

/* Well-known name for a port, e.g. "https" for 443. NULL when unknown. */
const char *netdash_port_service(uint16_t port);

/*
 * Human-facing form of a service id, for a label rather than a JSON field:
 * "smb" becomes "SMB", "home-assistant" becomes "Home Assistant", "portainer"
 * becomes "Portainer". Always NUL-terminates out.
 */
void netdash_service_label(const char *service, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* Availability history                                                      */
/* ------------------------------------------------------------------------- */

#define NETDASH_HISTORY_SLOT_SEC 300   /* one sample per five minutes        */
#define NETDASH_HISTORY_SLOTS    288   /* ...so the window is 24 hours       */
#define NETDASH_HISTORY_BYTES    (NETDASH_HISTORY_SLOTS / 8)

/*
 * One bit per five-minute slot: set when the device answered at least once
 * during that slot. Slots are cut on wall-clock time, so a device answering
 * twice in five minutes still only lights one bit and a slower sweep interval
 * simply leaves gaps rather than shifting the axis.
 *
 * Nothing is recorded before NTP has synced, because a slot number derived
 * from a wrong clock would put the samples in the wrong place. This lives in
 * RAM only: 24 hours of history is not worth the flash writes, and it fills
 * back up within a day of a reboot.
 *
 * out receives NETDASH_HISTORY_BYTES with the OLDEST slot in bit 0 of out[0],
 * which is the order a sparkline wants to draw. *out_valid is how many of the
 * 288 slots have actually elapsed, so a chart can show a short history as
 * short rather than as 24 hours of downtime.
 */
bool device_db_get_history(const uint8_t mac[6], uint8_t *out, size_t cap, uint16_t *out_valid);

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
