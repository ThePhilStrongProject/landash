/*
 * NetDash memory for mbedTLS. See tls_mem.h for why this exists.
 */
#include "tls_mem.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/platform.h"
#include "sdkconfig.h"

#ifndef CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
#error "tls_mem.c is the mbedTLS allocator: set CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC"
#endif

_Static_assert(TLS_MEM_RESERVE_BYTES >= CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN + 512,
               "the reserve must hold a whole TLS read buffer");

static uint8_t      s_reserve[TLS_MEM_RESERVE_BYTES] __attribute__((aligned(16)));
static bool         s_taken;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static tls_mem_stats_t s_stats;

static void *tls_mem_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    const size_t len = n * size;

    if (len > TLS_MEM_BIG_MIN && len <= sizeof(s_reserve)) {
        bool mine = false;
        taskENTER_CRITICAL(&s_mux);
        if (!s_taken) {
            s_taken = true;
            mine    = true;
            s_stats.reserve_uses++;
        } else {
            s_stats.reserve_busy++;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (mine) {
            memset(s_reserve, 0, len);
            return s_reserve;
        }
    }

    void *p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL && len > 0) {
        taskENTER_CRITICAL(&s_mux);
        s_stats.failures++;
        taskEXIT_CRITICAL(&s_mux);
    }
    return p;
}

static void tls_mem_free(void *ptr)
{
    if (ptr == (void *)s_reserve) {
        taskENTER_CRITICAL(&s_mux);
        s_taken = false;
        taskEXIT_CRITICAL(&s_mux);
        return;
    }
    heap_caps_free(ptr);
}

esp_err_t tls_mem_init(void)
{
    return mbedtls_platform_set_calloc_free(tls_mem_calloc, tls_mem_free) == 0 ? ESP_OK
                                                                              : ESP_FAIL;
}

void tls_mem_get_stats(tls_mem_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_mux);
}
