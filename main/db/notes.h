/*
 * NetDash per-device notes, and the vault that holds the secret ones.
 *
 * Two separate things live here because they share a lifetime (both are keyed
 * by MAC and both die with the device) but not a threat model:
 *
 *   notes   Plain text, NVS namespace "note". Served to anyone who can reach
 *           the web UI, exactly like a nickname. For "IPMI is on .211".
 *
 *   secrets AES-256-GCM ciphertext, NVS namespace "sec". Only ever decrypted
 *           after someone has unlocked the vault with the passphrase, and the
 *           key is derived from that passphrase every time - it is never
 *           stored. For "root / hunter2".
 *
 * What the vault is for, stated plainly so nobody over-trusts it:
 *
 *   It protects   Casual browsing of the web UI (a locked vault shows nothing
 *                 but a count), the device DB export, and a physical flash
 *                 dump, which yields PBKDF2-hardened ciphertext.
 *
 *   It does not   Protect against anyone who can watch LAN traffic while the
 *                 vault is unlocked. The dashboard is served over plain HTTP,
 *                 so the passphrase and any secret you reveal cross the
 *                 network in the clear. On a switched home LAN that needs
 *                 active ARP spoofing, which is a different sort of attacker
 *                 to the one this is for.
 *
 * Doing the crypto in the browser instead would fix that, and was the first
 * design tried: WebCrypto is unavailable here because crypto.subtle is gated
 * behind a secure context and http://landash.local is not one.
 *
 * The vault relocks itself after NETDASH_VAULT_IDLE_S of no use, and on every
 * reboot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETDASH_NOTE_MAX        256  /* including the NUL */
#define NETDASH_LINK_NOTE_MAX   160  /* including the NUL */
#define NETDASH_SECRET_MAX      192  /* plaintext, including the NUL */
#define NETDASH_VAULT_TOKEN_LEN 33   /* 32 hex characters plus the NUL */
#define NETDASH_VAULT_IDLE_S    900  /* relock after 15 minutes unused */
#define NETDASH_VAULT_PASS_MIN  8

/* Loads the note and secret indexes. Safe to call before Wi-Fi is up. */
esp_err_t notes_init(void);

/* ------------------------------------------------------------------------- */
/* Plain text notes                                                          */
/* ------------------------------------------------------------------------- */

/* NULL or "" erases the note. */
esp_err_t notes_set(const uint8_t mac[6], const char *text);

/* Copies the note into out. False when there is none; out is still NUL-set. */
bool notes_get(const uint8_t mac[6], char *out, size_t cap);

bool notes_exists(const uint8_t mac[6]);

/* Erases both the note and the secret for mac. Used by DELETE /api/devices. */
esp_err_t notes_forget_device(const uint8_t mac[6]);

/* ------------------------------------------------------------------------- */
/* Notes on dashboard links                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A note against a link rather than a device, because a link is a service and
 * a device may run several: "admin / see Bitwarden" belongs to the Portainer
 * tile, not to the whole NAS.
 *
 * Stored in their own NVS namespace keyed by the link id, not inline in the
 * links blob - notes are sparse, that blob is rewritten on every reordering,
 * and widening netdash_link_t would mean migrating the layout again.
 *
 * These are plain text and are served to anyone who can reach the web UI, the
 * same as a device note. Secrets belong in the vault.
 */

/* NULL or "" erases the note. */
esp_err_t link_note_set(uint16_t link_id, const char *text);

/* Copies the note into out. False when there is none; out is still NUL-set. */
bool link_note_get(uint16_t link_id, char *out, size_t cap);

bool link_note_exists(uint16_t link_id);

/* Erases the note for a link that is being deleted. */
esp_err_t link_note_forget(uint16_t link_id);

/*
 * Credentials against a link. A device may run half a dozen services, each
 * with its own login, so the service is the useful unit - the device-level
 * secret below is for the box itself, such as a console or BMC password.
 *
 * Same vault, same key, same rules: the vault must be unlocked, and rotating
 * the passphrase re-encrypts these along with everything else.
 */
esp_err_t link_secret_set(uint16_t link_id, const char *text);
esp_err_t link_secret_get(uint16_t link_id, char *out, size_t cap);
bool      link_secret_exists(uint16_t link_id);
size_t    link_secret_count(void);

/* Erases the secret for a link that is being deleted. */
esp_err_t link_secret_forget(uint16_t link_id);

/* ------------------------------------------------------------------------- */
/* Vault                                                                     */
/* ------------------------------------------------------------------------- */

bool vault_configured(void);
bool vault_unlocked(void);

/* Seconds until the idle relock, 0 when locked. */
uint32_t vault_idle_remaining(void);

/*
 * Creates the vault, or changes its passphrase. old_pass must be correct when
 * a vault already exists and is ignored when it does not. Rotating re-encrypts
 * every stored secret, so it needs the old passphrase even if the vault is
 * already unlocked.
 *
 * ESP_ERR_INVALID_STATE when old_pass is wrong, ESP_ERR_INVALID_SIZE when
 * new_pass is shorter than NETDASH_VAULT_PASS_MIN.
 *
 * On success the vault is left unlocked and out_token receives the session
 * token, because the caller has just proved they own it and making them
 * unlock again would mean deriving the key a second time for nothing.
 */
esp_err_t vault_set_passphrase(const char *old_pass, const char *new_pass,
                               char *out_token, size_t cap);

/*
 * Derives the key, checks it against the stored verifier and, on success,
 * holds it in RAM and writes a fresh session token to out_token.
 * ESP_ERR_NOT_FOUND when no vault exists, ESP_ERR_INVALID_STATE on a wrong
 * passphrase. Deliberately slow: the derivation is the brute-force cost.
 */
esp_err_t vault_unlock(const char *pass, char *out_token, size_t cap);

/* Wipes the key and invalidates the token. */
void vault_lock(void);

/* True when token matches the live session; refreshes the idle timer. */
bool vault_token_valid(const char *token);

/*
 * Destroys the vault and every secret in it. This is the way out when the
 * passphrase is lost - the secrets are not recoverable, by design.
 */
esp_err_t vault_reset(void);

/* ------------------------------------------------------------------------- */
/* Secrets                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Encrypts text against mac and stores it. NULL or "" erases the secret.
 * ESP_ERR_INVALID_STATE when the vault is locked.
 */
esp_err_t secret_set(const uint8_t mac[6], const char *text);

/*
 * Decrypts the secret for mac into out.
 * ESP_ERR_INVALID_STATE when locked, ESP_ERR_NOT_FOUND when there is none,
 * ESP_ERR_INVALID_MAC when the ciphertext fails its authentication tag.
 */
esp_err_t secret_get(const uint8_t mac[6], char *out, size_t cap);

/* True when a secret is stored, whether or not the vault is unlocked. */
bool secret_exists(const uint8_t mac[6]);

/* How many devices have a secret. Shown while the vault is locked. */
size_t secret_count(void);

#ifdef __cplusplus
}
#endif
