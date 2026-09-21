/*
 * Over-the-air updates from a GitHub releases repository.
 *
 * The dongle reads latest.json from the repository compiled into it
 * (CONFIG_NETDASH_OTA_REPO, branch CONFIG_NETDASH_OTA_BRANCH) through
 * raw.githubusercontent.com. When the version it names is newer than the
 * running firmware, it downloads the image file it names into the idle OTA
 * slot, checks the image calls itself that version, and reboots into it.
 * Publishing is therefore a commit and a push; tools/release.py makes the
 * commit.
 *
 * Why the repository is a build setting and not a web setting: the dashboard
 * has no login, so anything it can change, anyone on the LAN can change. A
 * repository field there would let any of them install their own firmware. A
 * token, by contrast, is safe to accept from the page - the worst a stranger
 * can do with it is point the dongle at a repository it cannot read.
 *
 * The releases repository is public, so nothing needs a token.
 *
 * Rollback: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on, so a freshly
 * installed image boots in a pending state. ota_init() marks it good once it
 * has been up for a minute and, when Wi-Fi is configured, has got an address.
 * An image that crashes first, or never gets online within ten minutes, is
 * rolled back by the bootloader, and that version is never auto-installed
 * again. A manual install still can, deliberately.
 *
 * Memory: a TLS session with GitHub is the biggest thing this firmware ever
 * does. With mbedTLS's dynamic buffers it leaves about 55 KB of heap free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,     /* nothing checked yet this boot                 */
    OTA_STATE_CHECKING,
    OTA_STATE_UP_TO_DATE,
    OTA_STATE_AVAILABLE,    /* newer release found, not (yet) installed      */
    OTA_STATE_DOWNLOADING,
    OTA_STATE_REBOOTING,    /* installed; restarting into it                 */
    OTA_STATE_ERROR,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char        current[32];    /* running version, from git describe        */
    char        latest[32];     /* newest release tag, "" when unknown       */
    char        error[80];      /* last failure, "" when none                */
    char        rolled_back[32];/* version the bootloader rejected, or ""    */
    int64_t     last_check;     /* unix seconds, 0 = never / before NTP      */
    uint32_t    bytes_done;     /* download progress                         */
    uint32_t    bytes_total;    /* 0 when unknown                            */
    bool        auto_blocked;   /* latest will not auto-install, see reason  */
} ota_status_t;

/* Stable lowercase name for the REST API, e.g. "downloading". */
const char *ota_state_name(ota_state_t state);

/*
 * Starts the update task and arms the rollback check. Call after wifi_mgr and
 * http_server are up. Safe to call when updates are disabled in settings: the
 * task then only answers manual requests.
 */
esp_err_t ota_init(void);

/* Copies the current status. */
void ota_get_status(ota_status_t *out);

/* Asks the task to check GitHub now. Returns immediately. */
esp_err_t ota_check_now(void);

/*
 * Asks the task to install the latest release, checking first if needed.
 * Returns ESP_ERR_INVALID_STATE when a download is already running.
 */
esp_err_t ota_install_now(void);

/* The repository updates come from, "owner/name", or "" when none is built in. */
const char *ota_repo(void);

#ifdef __cplusplus
}
#endif
