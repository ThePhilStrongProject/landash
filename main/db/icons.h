/*
 * Uploaded dashboard icons.
 *
 * Small PNGs kept in the "storage" SPIFFS partition, one file per icon, so a
 * link can wear the actual logo of whatever it points at rather than one of
 * the drawn glyphs in the page's sprite sheet.
 *
 * The browser does the hard part. It decodes whatever the user picked - PNG,
 * JPEG, WebP, a screenshot, a 4000px photo - draws it into a 64x64 canvas and
 * uploads the result as PNG. That means the firmware only ever sees a small
 * PNG of known dimensions, and never has to decode, rescale or sniff anything.
 * It still checks, because the browser is not the only thing that can POST.
 *
 * Budget: 48 icons at 16 KB each is 768 KB against a 1 MB partition, and a
 * real 64x64 logo lands nearer 2-5 KB, so the practical limit is the count
 * rather than the space.
 *
 * Ids are never reused, which is what lets GET /api/icons/{id} be served with
 * a one-year immutable cache: the bytes behind an id can never change.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETDASH_ICON_PX         64      /* what the browser resizes to      */
#define NETDASH_ICON_MAX_BYTES  16384   /* generous for a 64x64 PNG         */
#define NETDASH_MAX_ICONS       48      /* one per link, at most            */

/*
 * Mounts the storage partition, formatting it on first use, and indexes what
 * is already there. Returns ESP_OK even when the mount fails: uploaded icons
 * are a convenience, and losing them must not stop the dashboard booting.
 * icons_available() reports the truth.
 */
esp_err_t icons_init(void);

bool icons_available(void);

/*
 * Pulls an icon off the wire and stores it.
 *
 * read_fn is called repeatedly to fill buf and must return the number of bytes
 * read, 0 at end of input, or negative on error - the same shape as
 * httpd_req_recv(), so the HTTP layer can pass a thin wrapper.
 *
 * ESP_ERR_INVALID_SIZE when the upload is empty or over the cap,
 * ESP_ERR_INVALID_RESPONSE when it is not a PNG of at most
 * NETDASH_ICON_PX square, ESP_ERR_NO_MEM when the icon limit is reached,
 * ESP_ERR_INVALID_STATE when storage is unavailable. Nothing is left behind
 * on any failure path.
 */
esp_err_t icons_store(int (*read_fn)(void *ctx, char *buf, size_t len), void *ctx,
                      uint16_t *out_id);

size_t icons_count(void);

/* Copies the icon at index (0 .. icons_count()-1). False past the end. */
bool icons_get_at(size_t index, uint16_t *out_id, size_t *out_bytes);

bool icons_exists(uint16_t id);

esp_err_t icons_delete(uint16_t id);

/*
 * Opens an icon for reading and reports its size. NULL when it does not
 * exist. The caller closes it with fclose().
 */
FILE *icons_open(uint16_t id, size_t *out_bytes);

/* Bytes used by icons, and the usable size of the partition. */
void icons_usage(size_t *out_used, size_t *out_total);

#ifdef __cplusplus
}
#endif
