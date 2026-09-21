/*
 * NetDash notification feed.
 *
 * This is deliberately not the events log. The events log in device_db is a
 * raw trace of everything that happened; this is the much shorter list of
 * things worth telling the user about, with a read/unread state so the feed
 * can answer "what changed since I last looked" rather than "what happened".
 *
 * Every type can be switched off individually in settings (notif_mask), and a
 * type that is off is dropped at push time, so turning one off does not leave
 * old entries of that type stranded in the ring.
 *
 * The ring is persisted to NVS namespace "notif" so a reboot does not erase
 * the answer to "what changed while I was away". Writes are coalesced by a
 * timer, because a sweep can produce a burst.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETDASH_NOTIF_NEW_DEVICE = 0,
    NETDASH_NOTIF_IP_CHANGED,
    NETDASH_NOTIF_NEW_PORT,
    NETDASH_NOTIF_DEVICE_GONE,
    NETDASH_NOTIF_DEVICE_BACK,
    NETDASH_NOTIF_WAN_DOWN,
    NETDASH_NOTIF_WAN_UP,
    NETDASH_NOTIF_UPDATE,       /* firmware update found, installed, rolled back */
    NETDASH_NOTIF_COUNT
} netdash_notif_type_t;

#define NETDASH_NOTIF_BIT(t)   ((uint16_t)(1u << (t)))
#define NETDASH_NOTIF_ALL_MASK ((uint16_t)((1u << NETDASH_NOTIF_COUNT) - 1))

/*
 * Arrivals and departures are off by default. Phones, laptops and anything
 * with a sleep timer come and go all day, and a feed that cries wolf every
 * time one does is a feed nobody reads. Everything else is on.
 */
#define NETDASH_NOTIF_DEFAULT_MASK                                           \
    ((uint16_t)(NETDASH_NOTIF_ALL_MASK &                                     \
                ~(NETDASH_NOTIF_BIT(NETDASH_NOTIF_DEVICE_GONE) |             \
                  NETDASH_NOTIF_BIT(NETDASH_NOTIF_DEVICE_BACK))))

#define NETDASH_NOTIF_RING 48
#define NETDASH_NOTIF_TEXT 56

typedef struct __attribute__((packed)) {
    uint32_t id;                      /* monotonic, never reused            */
    int64_t  ts;                      /* unix seconds, 0 before NTP sync    */
    uint8_t  type;                    /* netdash_notif_type_t               */
    uint8_t  read;                    /* 0 = unread                         */
    uint8_t  mac[6];                  /* all zero when not device scoped    */
    uint32_t ip;                      /* 0 when not applicable              */
    char     text[NETDASH_NOTIF_TEXT];
} netdash_notif_t;

/* Loads the persisted ring and subscribes to the device events. */
esp_err_t notify_init(void);

/*
 * Appends a notification, oldest first out. Silently does nothing when the
 * type is disabled in settings or the feed has not been armed yet. mac may be
 * NULL, text may be NULL. Safe to call from any task.
 */
void notify_push(netdash_notif_type_t type, const uint8_t mac[6], uint32_t ip, const char *text);

/* index 0 is the newest entry. False past the end. */
bool notify_get(size_t index, netdash_notif_t *out);

size_t notify_count(void);
size_t notify_unread(void);

/* Marks one notification read, or every one of them when id is 0. */
esp_err_t notify_mark_read(uint32_t id);

/* Drops one notification, or the whole feed when id is 0. */
esp_err_t notify_dismiss(uint32_t id);

/*
 * Until the feed is armed, pushes are dropped. The scanner arms it when the
 * first sweep finishes, so a device that was simply not known yet at boot -
 * which on a freshly flashed dongle is every device on the network - does not
 * arrive as news.
 */
void notify_arm(void);
bool notify_armed(void);

/* Stable lowercase name used by the REST API, e.g. "new_device". */
const char *netdash_notif_type_name(netdash_notif_type_t type);

/* Parses a name produced by netdash_notif_type_name(). */
bool netdash_notif_type_from_name(const char *name, netdash_notif_type_t *out);

#ifdef __cplusplus
}
#endif
