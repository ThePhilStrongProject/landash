/*
 * NetDash memory for mbedTLS: one block set aside for the TLS read buffer.
 *
 * With CONFIG_MBEDTLS_DYNAMIC_BUFFER, mbedTLS allocates a fresh 16.7 KB buffer
 * for every incoming 16 KB TLS record and frees it once the record is read.
 * That needs 16.7 KB of *contiguous* free heap about a hundred times a
 * megabyte, and Wi-Fi receive buffers, lwIP and everything else are
 * allocating and freeing around it the whole time. By v0.20 the heap had
 * 70 KB free but no free block much over 30 KB, and an update died every few
 * records with "Dynamic Impl: alloc(16749 bytes) failed" until its retries ran
 * out - on the firmware doing the downloading, so no update could fix it.
 *
 * So CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC is set and this module is the allocator,
 * registered by tls_mem_init().
 * An allocation above TLS_MEM_BIG_MIN bytes takes the reserved block when it
 * is free; everything else, and a big one while the block is taken, comes from
 * internal RAM exactly as CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC gave it. The price
 * is TLS_MEM_RESERVE_BYTES of RAM held for good, in exchange for a download
 * that no longer depends on how fragmented the heap happens to be.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The read buffer asked for 16,749 bytes on IDF 5.5 with 16 KB records. */
#define TLS_MEM_RESERVE_BYTES (17 * 1024)

/* Anything smaller is left to the heap: the reserve is for record buffers. */
#define TLS_MEM_BIG_MIN (8 * 1024)

/*
 * Hands mbedTLS this allocator. Custom mode does not look it up by name: until
 * this runs mbedTLS uses plain calloc(), so it goes first in app_main, before
 * Wi-Fi or anything else can open a TLS connection.
 */
esp_err_t tls_mem_init(void);

typedef struct {
    uint32_t reserve_uses;   /* big allocations the reserved block served  */
    uint32_t reserve_busy;   /* big allocations that found it taken        */
    uint32_t failures;       /* allocations of any size that returned NULL */
} tls_mem_stats_t;

/* Counters since boot, for the heartbeat log. */
void tls_mem_get_stats(tls_mem_stats_t *out);

#ifdef __cplusplus
}
#endif
