/*
 * NetDash LCD: ST7789 240x135 over SPI, driven by LVGL 9 through
 * esp_lvgl_port, with LEDC backlight dimming.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_PAGE_MAIN = 0,      /* IP, mDNS name, SSID and RSSI             */
    DISPLAY_PAGE_STATS,         /* online / total / new, sweep progress     */
    DISPLAY_PAGE_AP,            /* AP SSID, password, http://192.168.4.1    */
    DISPLAY_PAGE_COUNT
} display_page_t;

/*
 * Brings up the panel, LVGL and the backlight, then shows DISPLAY_PAGE_MAIN,
 * and subscribes to NETDASH_EVENT so the pages keep themselves up to date.
 *
 * A missing or mis-wired panel must not stop the rest of the firmware, so this
 * always returns ESP_OK: a panel failure is logged at ERROR, display_is_ready()
 * then reports false and every other entry point below becomes a no-op.
 */
esp_err_t display_init(void);

/* False when the panel did not come up; all calls below are then no-ops. */
bool display_is_ready(void);

/* Switches the visible page and wakes the backlight. Safe from any task. */
esp_err_t display_show_page(display_page_t page);

/*
 * Advances to the next page and wakes the backlight (what a short press on the
 * BOOT button does). DISPLAY_PAGE_AP is only included in the cycle while the
 * softAP is up. Safe from any task.
 */
esp_err_t display_next_page(void);

/* Restores full brightness and restarts the idle-dim timer. Any task. */
void display_wake(void);

/*
 * BOOT-hold feedback: secs_left > 0 shows "Hold to reset Wi-Fi... <n>" over the
 * current page, secs_left <= 0 removes it. Safe from any task.
 */
void display_hold_countdown(int secs_left);

/* Shows a 5 s toast over the current page. Safe from any task; text is copied. */
void display_toast(const char *text);

#ifdef __cplusplus
}
#endif
