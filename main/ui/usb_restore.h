/*
 * The USB end of a restore: lets the web installer hand a .landash file to a
 * dongle it has just flashed, over the same cable, so a replacement comes up
 * on the home Wi-Fi without the user ever joining its setup network.
 *
 * The console already runs over USB Serial/JTAG. This installs the driver
 * under it so there is a receive buffer to read from. Logging carries on
 * through the driver, and with no host listening a write is dropped after one
 * short wait rather than blocking, so a dongle on a phone charger is
 * unaffected.
 *
 * The protocol is lines, with the log running in between them, so every line
 * of ours starts with "LANDASH:" and the host ignores everything else:
 *
 *   host    LANDASH-RESTORE <bytes>\n
 *   dongle  LANDASH:READY                  once anything queued behind the
 *                                          command has been drained
 *   host    exactly <bytes> bytes: the body of POST /api/restore - the
 *           passphrase, a newline, then the .landash file unchanged - never
 *           more than 3072 of them beyond the last acknowledgement
 *   dongle  LANDASH:ACK <n>                every 1024 bytes it has taken in,
 *                                          n counting from the first byte
 *   dongle  LANDASH:OK {json}              what POST /api/restore returns, then
 *                                          it restarts and applies the backup
 *       or  LANDASH:ERR <status> <message> the status POST /api/restore would
 *                                          use, e.g. "403 Wrong passphrase."
 *
 * The acknowledgements are the flow control. USB itself would hold the host
 * back, but the driver empties the hardware FIFO into a 4 KB ring buffer on
 * every packet and drops what does not fit, and the dongle stops reading for
 * several seconds while it derives the key. Without them a restore lost
 * bytes and timed out part-way.
 *
 * And, whether or not a restore is running, every time the station gets an
 * address (and once at start-up if it already has one):
 *
 *   dongle  LANDASH:IP <a.b.c.d>
 *
 * which is how the installer tells the user where the restored dongle is.
 * docs/API.md describes the same.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Installs the USB Serial/JTAG driver and starts listening. Call last in
 * app_main: a restore borrows the idle update slot from net/ota.c, so it must
 * not be offered before ota_init().
 */
esp_err_t usb_restore_init(void);

#ifdef __cplusplus
}
#endif
