/*
 * Backup and restore: one .landash file that sets up a replacement dongle
 * exactly like the one it came from.
 *
 * What it carries: every NVS namespace a user's work lives in (settings with
 * the Wi-Fi password and the setup-AP password, dashboard links and groups,
 * notes, the vault and its secrets, the icon id counter) and every file on the
 * storage partition (the device register and the uploaded icons). What it
 * leaves out, on purpose: the notification feed and the update bookkeeping,
 * which describe the old dongle's life rather than the user's setup.
 *
 * Entries and files are carried byte for byte, not re-expressed as fields.
 * Restoring an entry is then exactly what an update does to it - the new
 * firmware meets a blob an older one wrote - so every module's existing
 * version handling does the work. The price is a promise: a loader must go on
 * accepting every layout written since backups began (v0.19.0), not just the
 * one it replaced. docs/API.md has the file format.
 *
 * The file is encrypted with a passphrase chosen when it is made: PBKDF2 for
 * the key, AES-256-GCM for each record separately with its index in the IV,
 * and a hash of the readable header as AAD. So the whole file is checked
 * record by record as it streams in, and a truncated, reordered or edited file
 * is refused. The vault's secrets are inside it still encrypted under the
 * vault passphrase; the backup passphrase does not reveal them.
 *
 * Restoring is two steps with a reboot between. backup_restore() checks every
 * record and stages the plaintext in the idle update slot (net/ota.h lends
 * it), changing nothing. backup_apply_pending(), first thing at the next boot,
 * wipes and rewrites the namespaces and the storage partition before any
 * module has read them, so no running module is left holding stale state. A
 * power cut while applying just applies it again on the following boot.
 *
 * The dashboard has no login, so anyone on the LAN can make a backup - and
 * with it the Wi-Fi password - or restore one. That is the same exposure as
 * the rest of the API, stated in docs/API.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKUP_PASS_MIN  8
#define BACKUP_PASS_MAX  128
#define BACKUP_MAX_BYTES (4u * 1024u * 1024u)   /* the largest file restore takes */

/* What a backup's readable header says about it. */
typedef struct {
    char     fw[32];
    char     hostname[32];
    int64_t  created;       /* unix seconds, 0 when the clock had not synced */
    uint32_t devices;
    uint32_t links;
    uint32_t icons;
} backup_info_t;

typedef struct {
    int64_t       last_backup;  /* unix seconds of the last one made, 0 = never */
    bool          restored;     /* this dongle was set up from a backup...     */
    backup_info_t from;         /* ...and this is the one                      */
} backup_status_t;

/*
 * Applies a restore staged by backup_restore(), if there is one, then clears
 * it. Call once at boot after nvs_flash_init() and before any module that
 * reads NVS or mounts the storage partition (settings_init(), icons_init()).
 * Does nothing, quickly, when nothing is staged. A failure is logged and the
 * dongle boots on whatever is there.
 */
void backup_apply_pending(void);

/* True when the passphrase is usable: length in range, no control characters. */
bool backup_passphrase_ok(const char *pass);

/*
 * The file name a backup made at now should be saved under:
 * "landash-<hostname>-<YYYYMMDD>.landash", or without the date when now is 0
 * (the clock has not synced).
 */
void backup_filename(int64_t now, char *out, size_t cap);

/*
 * Writes a complete backup through write_fn, which returns ESP_OK or an error
 * that aborts the backup. The first call comes after the key derivation,
 * about six seconds in, so a caller can still report a failure before it cleanly.
 * now is the unix time to stamp it with, 0 before NTP has synced; on success
 * it is recorded for backup_get_status(). ESP_ERR_INVALID_ARG for a
 * passphrase backup_passphrase_ok() rejects.
 */
typedef esp_err_t (*backup_write_fn)(void *ctx, const void *buf, size_t len);
esp_err_t backup_export(const char *passphrase, int64_t now, backup_write_fn write_fn,
                        void *ctx);

/*
 * Reads a restore request through read_fn - same contract as
 * httpd_req_recv(): bytes read, 0 at the end, negative on error. The stream
 * is the passphrase, one newline (0x0A), then the .landash file unchanged; HTTP
 * and any later transport share that framing. Checks every record and stages
 * it for backup_apply_pending(). Nothing is changed until the caller reboots,
 * and not at all on failure. On success *out_info describes the backup.
 *
 *   ESP_ERR_INVALID_ARG       not a LANDA.SH backup (or a bad passphrase)
 *   ESP_ERR_INVALID_MAC       wrong passphrase: the first record fails its tag
 *   ESP_ERR_INVALID_CRC       damaged or incomplete after that
 *   ESP_ERR_NOT_SUPPORTED     made by newer firmware, or a newer file format
 *   ESP_ERR_INVALID_STATE     the idle update slot is not free to borrow
 *   ESP_ERR_INVALID_SIZE      over BACKUP_MAX_BYTES
 *   ESP_ERR_TIMEOUT / ESP_FAIL  the read failed / a flash write failed
 *
 * err receives a sentence for a person in every failure case.
 */
typedef int (*backup_read_fn)(void *ctx, void *buf, size_t len);
esp_err_t backup_restore(backup_read_fn read_fn, void *ctx, backup_info_t *out_info, char *err,
                         size_t err_cap);

/* When the last backup was made, and what this dongle was restored from. */
void backup_get_status(backup_status_t *out);

#ifdef __cplusplus
}
#endif
