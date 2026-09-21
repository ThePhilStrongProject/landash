/*
 * The device register: every device ever seen, in flash.
 *
 * device_db keeps the devices that matter right now - online, or seen
 * recently - in a table in RAM, and that table is small because RAM is. This
 * is where everything else lives: one fixed-size record per device in a single
 * file on the "storage" SPIFFS partition (mounted by db/icons.c), so a busy
 * network is not limited to what fits in RAM, and a device that has been gone
 * for a month comes back with its nickname.
 *
 * Part of the device_db module: nothing else should call it. device_db owns
 * the rules about what goes in and when; this file only stores records.
 *
 * Records are addressed by the device's key - the 6-byte id device_db uses,
 * which is the MAC, or a synthetic id for a device behind a shared MAC. A
 * 4-byte fingerprint of every slot's key is kept in RAM, so finding a record
 * costs one flash read, not a scan of the file.
 *
 * Every function takes the store's own mutex and does file I/O. Never call
 * one while holding device_db_lock().
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

#define DEV_STORE_MAX   CONFIG_NETDASH_REGISTER_DEVICES
#define DEV_STORE_PORTS 24      /* open ports kept per device               */

/*
 * One device, as stored. Fixed at 192 bytes: never reorder or resize a field,
 * only take bytes from the reserved tail and bump DEV_REC_VERSION.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;             /* DEV_REC_MAGIC while in use                */
    uint8_t  version;
    uint8_t  flags;             /* NETDASH_FLAG_*                            */
    uint8_t  type_override;
    uint8_t  type;              /* last automatic classification             */
    uint8_t  name_src;
    uint8_t  sources;
    uint8_t  port_count;
    uint8_t  port_tier;         /* highest port-scan tier finished           */
    uint8_t  reserved0[3];
    uint8_t  mac[6];            /* the key                                   */
    uint8_t  hw_mac[6];
    uint32_t last_ip;
    uint16_t services;
    uint16_t reserved1;
    int64_t  first_seen;
    int64_t  last_seen;
    int64_t  port_last_scan;
    char     nickname[32];
    char     hostname[32];
    uint16_t ports[DEV_STORE_PORTS];
    uint8_t  reserved2[24];
} dev_rec_t;

#define DEV_REC_MAGIC   0x5D
#define DEV_REC_VERSION 1

/*
 * Opens the register, creating it when absent. *out_created is set when the
 * file did not exist - the moment to migrate older storage into it. Needs the
 * storage partition mounted (icons_init() first). ESP_ERR_INVALID_STATE when
 * the partition is unavailable: device_db then runs on RAM alone.
 */
esp_err_t dev_store_init(bool *out_created);

bool dev_store_available(void);

/* Records in use. */
size_t dev_store_count(void);

/* Slots in the file, used or free; the bound for dev_store_read_slot(). */
size_t dev_store_slots(void);

/* The record for id. False when there is none (or no register). */
bool dev_store_get(const uint8_t id[6], dev_rec_t *out);

/* The record in slot i. False when the slot is free or past the end. */
bool dev_store_read_slot(size_t i, dev_rec_t *out);

/*
 * Writes rec, replacing the record with the same key or taking a free slot.
 * When the register is full, the record not seen for longest that has no
 * nickname makes way, and *out_evicted (may be NULL) receives its key so the
 * caller can forget what else was kept about it; *out_did_evict says whether
 * that happened. ESP_ERR_NO_MEM when nothing could make way.
 */
esp_err_t dev_store_put(const dev_rec_t *rec, uint8_t out_evicted[6], bool *out_did_evict);

/*
 * Read-modify-write of id's record in one step, under the store's lock, so two
 * tasks editing different fields of the same device cannot undo each other.
 * fn gets the current record - or, when there is none and create is true, a
 * blank one with the key filled in and *created set - and returns true to
 * have it written back. ESP_ERR_NOT_FOUND when there is no record and create
 * is false. Eviction as for dev_store_put(). fn runs with the store locked:
 * it must not call back into the store.
 */
typedef bool (*dev_store_edit_fn)(dev_rec_t *rec, bool created, void *ctx);
esp_err_t dev_store_update(const uint8_t id[6], bool create, dev_store_edit_fn fn, void *ctx,
                           uint8_t out_evicted[6], bool *out_did_evict);

/* Frees id's slot. ESP_OK when there was nothing to free. */
esp_err_t dev_store_remove(const uint8_t id[6]);

#ifdef __cplusplus
}
#endif
