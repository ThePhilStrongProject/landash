/*
 * NetDash LCD front end.
 *
 * Panel: ST7789 240x135 IPS over 4-wire SPI (no MISO). Pin map, SPI clock,
 * colour inversion and the panel offsets were confirmed against Waveshare's
 * own ESP32-C6-GEEK-Demo.zip:
 *
 *   ESP-IDF/06_lvgl_example_v9/main/LCD_Driver/lcd_st7789.h:13-33
 *       X_GAP 52, Y_GAP 40, SPI2_HOST, 40 MHz, 8/8 cmd/param bits,
 *       BL_ON_LEVEL 1, SCLK 1, MOSI 2, RST 4, DC 3, CS 5, BL 6
 *   ESP-IDF/06_lvgl_example_v9/main/LCD_Driver/lcd_st7789.c:56-64
 *       reset, init, set_gap(52, 40), invert_color(true), disp_on
 *   Arduino/examples/05_LCD_Button/LCD_Driver.cpp:95-96, 165, 183-186
 *       MADCTL (0x36) = 0x70 = MX | MV | ML, INVON (0x21), +52 on CASET
 *
 * Orientation: the demo runs the panel portrait (135x240) with swap_xy off and
 * gaps 52/40. NetDash wants 240x135 landscape, so MADCTL gets the MV bit
 * (swap_xy) plus MX (mirror_x) - the same MX|MV the Arduino driver writes. MV
 * transposes the controller's CASET/RASET addressing, so the gaps swap too and
 * become x=40 / y=52, which is what Kconfig defaults to.
 * NETDASH_LCD_ROTATE_180 selects MV|MY instead, for a board mounted the other
 * way up.
 *
 * Colour bytes: RGB565 over an 8-bit SPI bus needs the two halves swapped, so
 * esp_lvgl_port runs with swap_bytes = true, exactly as the demo does.
 *
 * Threading: LVGL is only ever touched with the esp_lvgl_port lock held. The
 * periodic refresh runs as an lv_timer inside the LVGL task (which already
 * holds that lock), and the NETDASH_EVENT handler does no LVGL work at all -
 * it only parks values in s_state for the refresh to pick up. Lock order is
 * always LVGL -> s_state_mux -> device_db, never the other way round.
 */
#include "display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_events.h"
#include "device_db.h"
#include "scanner.h"
#include "settings.h"
#include "wan.h"
#include "wifi_mgr.h"

static const char *TAG = "display";

/* ------------------------------------------------------------------------- */
/* Panel / LVGL geometry                                                     */
/* ------------------------------------------------------------------------- */

#define LCD_H_RES            240        /* landscape width  */
#define LCD_V_RES            135        /* landscape height */
#define LCD_DRAW_LINES       20         /* partial buffer height, in lines */
#define LCD_PIXEL_CLOCK_HZ   (40 * 1000 * 1000)
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8
#define LCD_SPI_HOST         SPI2_HOST

/* Backlight (LEDC, 5 kHz, 8 bit). */
#define BL_TIMER             LEDC_TIMER_0
#define BL_CHANNEL           LEDC_CHANNEL_0
#define BL_MODE              LEDC_LOW_SPEED_MODE
#define BL_RESOLUTION        LEDC_TIMER_8_BIT
#define BL_FREQ_HZ           5000
#define BL_LEVEL_FULL        255
#define BL_LEVEL_DIM         25         /* ~10 % */

#define REFRESH_PERIOD_MS    250
#define TOAST_MS             5000
#define NEW_DEVICE_WINDOW_S  (24 * 60 * 60)

/*
 * Kconfig bools are undefined rather than 0 when turned off, so the ones used
 * as C values (not just in #if) get a plain macro here.
 */
#ifdef CONFIG_NETDASH_LCD_INVERT_COLORS
#define LCD_INVERT_COLORS    true
#else
#define LCD_INVERT_COLORS    false
#endif

/* Theme. */
#define COL_BG               0x05080F
#define COL_SURFACE          0x0C1220
#define COL_BORDER           0x1E293B
#define COL_ACCENT           0x3B82F6
#define COL_TEXT             0xF1F5F9
#define COL_MUTED            0x94A3B8
#define COL_OK               0x22C55E
#define COL_WARN             0xF59E0B
#define COL_BAD              0xEF4444

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static bool s_ready;                    /* panel + LVGL came up */
static bool s_backlight;                /* LEDC configured */
static bool s_dimmed;

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

static display_page_t s_page = DISPLAY_PAGE_MAIN;
static int64_t s_last_activity_us;

/* Pages and widgets. */
static lv_obj_t *s_pages[DISPLAY_PAGE_COUNT];
static lv_obj_t *s_rssi_bars[4];
static lv_obj_t *s_lbl_ip, *s_lbl_host, *s_lbl_ssid, *s_lbl_wan;
static lv_obj_t *s_lbl_online, *s_lbl_total, *s_lbl_new, *s_lbl_sweep;
static lv_obj_t *s_bar_scan;
static lv_obj_t *s_lbl_ap_ssid, *s_lbl_ap_pass;
static lv_obj_t *s_toast, *s_toast_lbl;
static lv_obj_t *s_hold, *s_hold_lbl;

static int64_t s_toast_until_us;

/* Written by the event handler / other tasks, drained by the refresh timer. */
static SemaphoreHandle_t s_state_mux;
static struct {
    char     toast[48];
    bool     toast_pending;
    int      want_page;                 /* -1 = none */
    bool     scanning;
    uint16_t scan_done;
    uint16_t scan_total;
    bool     ap_up;
} s_state = { .want_page = -1 };

static void state_lock(void)
{
    if (s_state_mux != NULL) {
        xSemaphoreTake(s_state_mux, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (s_state_mux != NULL) {
        xSemaphoreGive(s_state_mux);
    }
}

/* ------------------------------------------------------------------------- */
/* Backlight                                                                 */
/* ------------------------------------------------------------------------- */

static void backlight_set(uint8_t level)
{
    if (!s_backlight) {
        return;
    }
#if CONFIG_NETDASH_LCD_BL_ACTIVE_LOW
    uint32_t duty = (uint32_t)(BL_LEVEL_FULL - level);
#else
    uint32_t duty = level;
#endif
    esp_err_t err = ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BL_MODE, BL_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight duty %u failed: %s", (unsigned)level, esp_err_to_name(err));
    }
}

static esp_err_t backlight_init(void)
{
#if CONFIG_NETDASH_PIN_LCD_BL >= 0
    const ledc_timer_config_t timer = {
        .speed_mode      = BL_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_RESOLUTION,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    const ledc_channel_config_t channel = {
        .gpio_num   = CONFIG_NETDASH_PIN_LCD_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");

    s_backlight = true;
    backlight_set(BL_LEVEL_FULL);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "no backlight GPIO configured");
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------------- */
/* Panel bring-up                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Paints black over the drawn area and one row/column of margin around it.
 *
 * The ST7789 carries a 240x320 frame buffer while this panel only shows
 * 240x135 of it, positioned by the configured gap. LVGL never writes the rows
 * just outside that window, so whatever the controller powered up with stays
 * there - which showed as a line of coloured noise along the bottom edge.
 * Clearing the margin once at boot fixes it permanently, and it does so
 * whether the exact gap is 52 or 53, which differ by one row between the two
 * landscape rotations.
 */
static void lcd_blank_margins(void)
{
    const int x0 = CONFIG_NETDASH_LCD_X_OFFSET > 0 ? CONFIG_NETDASH_LCD_X_OFFSET - 1 : 0;
    const int y0 = CONFIG_NETDASH_LCD_Y_OFFSET > 0 ? CONFIG_NETDASH_LCD_Y_OFFSET - 1 : 0;
    const int w  = LCD_H_RES + 2;
    const int h  = LCD_V_RES + 2;
    const int strip_rows = 16;

    uint16_t *strip = heap_caps_calloc((size_t)w * strip_rows, sizeof(uint16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (strip == NULL) {
        ESP_LOGW(TAG, "no DMA buffer for margin blanking, skipping");
        return;
    }
    /* calloc gives 0x0000, which is black in RGB565 either byte order. */

    /* Address the margin directly, so the window can start before the gap. */
    if (esp_lcd_panel_set_gap(s_panel, x0, y0) != ESP_OK) {
        free(strip);
        return;
    }

    for (int y = 0; y < h; y += strip_rows) {
        const int rows = (y + strip_rows > h) ? (h - y) : strip_rows;
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, w, y + rows, strip) != ESP_OK) {
            break;
        }
    }

    free(strip);
    (void)esp_lcd_panel_set_gap(s_panel, CONFIG_NETDASH_LCD_X_OFFSET,
                                CONFIG_NETDASH_LCD_Y_OFFSET);
}

static esp_err_t panel_init(void)
{
    esp_err_t ret = ESP_OK;

    const spi_bus_config_t bus = {
        .sclk_io_num     = CONFIG_NETDASH_PIN_LCD_SCLK,
        .mosi_io_num     = CONFIG_NETDASH_PIN_LCD_MOSI,
        .miso_io_num     = GPIO_NUM_NC,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_LINES * (int)sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = CONFIG_NETDASH_PIN_LCD_DC,
        .cs_gpio_num       = CONFIG_NETDASH_PIN_LCD_CS,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                               &io_cfg, &s_io),
                      fail, TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_NETDASH_PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), fail, TAG, "st7789");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel), fail, TAG, "reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel), fail, TAG, "init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLORS),
                      fail, TAG, "invert");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(s_panel, CONFIG_NETDASH_LCD_X_OFFSET,
                                            CONFIG_NETDASH_LCD_Y_OFFSET),
                      fail, TAG, "gap");
    lcd_blank_margins();
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), fail, TAG, "disp on");
    return ESP_OK;

fail:
    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io != NULL) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    spi_bus_free(LCD_SPI_HOST);
    return (ret == ESP_OK) ? ESP_FAIL : ret;
}

static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority   = 4,
        .task_stack      = 6144,
        .task_affinity   = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_io,
        .panel_handle  = s_panel,
        .buffer_size   = LCD_H_RES * LCD_DRAW_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            /* MADCTL MV | MX (or MV | MY when rotated 180). */
            .swap_xy  = true,
#if CONFIG_NETDASH_LCD_ROTATE_180
            .mirror_x = false,
            .mirror_y = true,
#else
            .mirror_x = true,
            .mirror_y = false,
#endif
        },
        .flags = {
            .buff_dma    = true,
            .swap_bytes  = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Widget helpers (all called with the LVGL lock held)                       */
/* ------------------------------------------------------------------------- */

static lv_obj_t *make_box(lv_obj_t *parent, int32_t w, int32_t h, uint32_t bg)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_scrollable(box, false);
    if (bg != COL_BG) {
        lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    }
    return box;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *page_create(void)
{
    lv_obj_t *page = make_box(lv_screen_active(), LCD_H_RES, LCD_V_RES, COL_BG);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_hidden(page, true);
    return page;
}

static void build_page_main(void)
{
    lv_obj_t *page = page_create();
    s_pages[DISPLAY_PAGE_MAIN] = page;

    lv_obj_t *hdr = make_label(page, &lv_font_montserrat_12, COL_ACCENT, "LANDA.SH");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 6, 6);

    for (int i = 0; i < 4; i++) {
        int32_t h = 5 + 3 * i;
        s_rssi_bars[i] = make_box(page, 3, h, COL_BORDER);
        lv_obj_set_style_radius(s_rssi_bars[i], 1, 0);
        lv_obj_align(s_rssi_bars[i], LV_ALIGN_TOP_RIGHT, -(15 - 5 * i), 20 - h);
    }

    s_lbl_ip = make_label(page, &lv_font_montserrat_28, COL_TEXT, "...");
    lv_obj_set_width(s_lbl_ip, LCD_H_RES - 8);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_lbl_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_ip, LV_ALIGN_CENTER, 0, -10);

    s_lbl_host = make_label(page, &lv_font_montserrat_14, COL_ACCENT, "");
    lv_obj_align(s_lbl_host, LV_ALIGN_CENTER, 0, 26);

    /*
     * The bottom row carries the SSID on the left and the WAN verdict on the
     * right. The dongle is plugged in and always on, so this is the line that
     * earns the screen: the IP can be looked up on the router, "is the
     * internet actually up" cannot.
     */
    s_lbl_ssid = make_label(page, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_width(s_lbl_ssid, LCD_H_RES - 116);
    lv_label_set_long_mode(s_lbl_ssid, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_lbl_ssid, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_BOTTOM_LEFT, 6, -6);

    s_lbl_wan = make_label(page, &lv_font_montserrat_12, COL_MUTED, "");
    lv_obj_set_style_text_align(s_lbl_wan, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_lbl_wan, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
}

static lv_obj_t *make_tile(lv_obj_t *page, int32_t x, const char *caption)
{
    lv_obj_t *tile = make_box(page, 74, 66, COL_SURFACE);
    lv_obj_set_style_radius(tile, 6, 0);
    lv_obj_set_pos(tile, x, 20);

    lv_obj_t *value = make_label(tile, &lv_font_montserrat_28, COL_TEXT, "0");
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *cap = make_label(tile, &lv_font_montserrat_12, COL_MUTED, caption);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -5);

    return value;
}

static void build_page_stats(void)
{
    lv_obj_t *page = page_create();
    s_pages[DISPLAY_PAGE_STATS] = page;

    lv_obj_t *hdr = make_label(page, &lv_font_montserrat_12, COL_ACCENT, "DEVICES");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 6, 4);

    s_lbl_online = make_tile(page, 4, "Online");
    s_lbl_total  = make_tile(page, 83, "Total");
    s_lbl_new    = make_tile(page, 162, "New 24h");

    s_bar_scan = lv_bar_create(page);
    lv_obj_set_size(s_bar_scan, LCD_H_RES - 16, 4);
    lv_obj_align(s_bar_scan, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(s_bar_scan, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_color(s_bar_scan, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_scan, 2, 0);
    lv_obj_set_style_radius(s_bar_scan, 2, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar_scan, 0, 100);
    lv_bar_set_value(s_bar_scan, 0, LV_ANIM_OFF);
    lv_obj_set_hidden(s_bar_scan, true);

    s_lbl_sweep = make_label(page, &lv_font_montserrat_12, COL_MUTED, "last sweep --:--");
    lv_obj_align(s_lbl_sweep, LV_ALIGN_BOTTOM_MID, 0, -5);
}

static void build_page_ap(void)
{
    lv_obj_t *page = page_create();
    s_pages[DISPLAY_PAGE_AP] = page;

    lv_obj_t *l1 = make_label(page, &lv_font_montserrat_12, COL_MUTED, "Join Wi-Fi:");
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 8, 6);

    s_lbl_ap_ssid = make_label(page, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_align(s_lbl_ap_ssid, LV_ALIGN_TOP_LEFT, 8, 20);

    lv_obj_t *l2 = make_label(page, &lv_font_montserrat_12, COL_MUTED, "Password:");
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 8, 50);

    s_lbl_ap_pass = make_label(page, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_align(s_lbl_ap_pass, LV_ALIGN_TOP_LEFT, 8, 64);

    lv_obj_t *l3 = make_label(page, &lv_font_montserrat_14, COL_ACCENT,
                              "then open http://192.168.4.1");
    lv_obj_align(l3, LV_ALIGN_BOTTOM_MID, 0, -6);
}

static void build_overlays(void)
{
    s_toast = make_box(lv_screen_active(), LCD_H_RES, 26, COL_ACCENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_toast, true);
    s_toast_lbl = make_label(s_toast, &lv_font_montserrat_14, COL_TEXT, "");
    lv_obj_set_width(s_toast_lbl, LCD_H_RES - 12);
    lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_toast_lbl);

    s_hold = make_box(lv_screen_active(), LCD_H_RES - 24, 56, COL_SURFACE);
    lv_obj_set_style_radius(s_hold, 8, 0);
    lv_obj_set_style_border_color(s_hold, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(s_hold, 2, 0);
    lv_obj_center(s_hold);
    lv_obj_set_hidden(s_hold, true);

    lv_obj_t *cap = make_label(s_hold, &lv_font_montserrat_12, COL_MUTED, "Hold to reset Wi-Fi...");
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 6);
    s_hold_lbl = make_label(s_hold, &lv_font_montserrat_28, COL_TEXT, "3");
    lv_obj_align(s_hold_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void apply_page(display_page_t page)
{
    for (int i = 0; i < DISPLAY_PAGE_COUNT; i++) {
        if (s_pages[i] == NULL) {
            continue;
        }
        if (i == (int)page) {
            lv_obj_set_hidden(s_pages[i], false);
        } else {
            lv_obj_set_hidden(s_pages[i], true);
        }
    }
    s_page = page;
}

/* ------------------------------------------------------------------------- */
/* Periodic refresh (runs in the LVGL task, which holds the LVGL lock)       */
/* ------------------------------------------------------------------------- */

static void refresh_main(void)
{
    esp_netif_ip_info_t info;
    bool have_ip = (wifi_mgr_get_ip_info(&info) == ESP_OK) && (info.ip.addr != 0);
    const char *ssid = wifi_mgr_get_ssid();

    char buf[80];
    if (have_ip) {
        lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_28, 0);
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    } else {
        lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_14, 0);
        snprintf(buf, sizeof(buf), "Connecting to %s...", (ssid[0] != '\0') ? ssid : "Wi-Fi");
    }
    lv_label_set_text(s_lbl_ip, buf);
    lv_obj_align(s_lbl_ip, LV_ALIGN_CENTER, 0, -10);

    netdash_settings_t cfg;
    settings_get(&cfg);
    snprintf(buf, sizeof(buf), "%s.local", cfg.hostname);
    lv_label_set_text(s_lbl_host, have_ip ? buf : "");
    lv_obj_align(s_lbl_host, LV_ALIGN_CENTER, 0, 26);

    lv_label_set_text(s_lbl_ssid, ssid);

    /*
     * A degraded WAN is spelled out as which half failed. "No DNS" and "WAN
     * down" call for completely different responses, and telling them apart
     * is the whole reason the check is two probes rather than one.
     */
    netdash_wan_t wan;
    wan_get(&wan);

    uint32_t wan_col = COL_MUTED;
    switch ((netdash_wan_state_t)wan.state) {
    case NETDASH_WAN_UP:
        wan_col = COL_OK;
        if (wan.rtt_ms >= 0) {
            snprintf(buf, sizeof(buf), "WAN %d ms", (int)wan.rtt_ms);
        } else {
            snprintf(buf, sizeof(buf), "WAN ok");
        }
        break;
    case NETDASH_WAN_DEGRADED:
        wan_col = COL_WARN;
        snprintf(buf, sizeof(buf), "%s", wan.icmp_ok ? "No DNS" : "No ICMP");
        break;
    case NETDASH_WAN_DOWN:
        wan_col = COL_BAD;
        snprintf(buf, sizeof(buf), "WAN down");
        break;
    default:
        buf[0] = 0;
        break;
    }
    lv_label_set_text(s_lbl_wan, buf);
    lv_obj_set_style_text_color(s_lbl_wan, lv_color_hex(wan_col), 0);
    lv_obj_align(s_lbl_wan, LV_ALIGN_BOTTOM_RIGHT, -6, -6);

    int8_t rssi = wifi_mgr_get_rssi();
    int strength = 0;
    if (rssi != 0) {
        if (rssi >= -55) {
            strength = 4;
        } else if (rssi >= -65) {
            strength = 3;
        } else if (rssi >= -75) {
            strength = 2;
        } else {
            strength = 1;
        }
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_rssi_bars[i],
                                  lv_color_hex(i < strength ? COL_ACCENT : COL_BORDER), 0);
    }
}

static void refresh_stats(void)
{
    time_t now = time(NULL);
    size_t total = 0, online = 0, fresh = 0;
    netdash_device_t dev;

    device_db_lock();
    for (size_t i = 0; device_db_get_at(i, &dev); i++) {
        total++;
        if (netdash_device_online(&dev)) {
            online++;
        }
        if (dev.first_seen != 0 && now > 0 &&
            ((int64_t)now - dev.first_seen) < NEW_DEVICE_WINDOW_S) {
            fresh++;
        }
    }
    device_db_unlock();

    /* Wide enough that GCC can prove the "last sweep %02d:%02d" case fits. */
    char buf[48];
    snprintf(buf, sizeof(buf), "%u", (unsigned)online);
    lv_label_set_text(s_lbl_online, buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)total);
    lv_label_set_text(s_lbl_total, buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)fresh);
    lv_label_set_text(s_lbl_new, buf);

    int64_t last = scanner_last_sweep_time();
    if (last > 0) {
        struct tm tm_buf;
        time_t t = (time_t)last;
        localtime_r(&t, &tm_buf);
        snprintf(buf, sizeof(buf), "last sweep %02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "last sweep --:--");
    }
    lv_label_set_text(s_lbl_sweep, buf);

    bool scanning;
    uint32_t done, all;
    state_lock();
    scanning = s_state.scanning;
    done = s_state.scan_done;
    all = s_state.scan_total;
    state_unlock();

    if (scanning) {
        lv_obj_set_hidden(s_bar_scan, false);
        int32_t pct = (all > 0) ? (int32_t)((done * 100U) / all) : 0;
        lv_bar_set_value(s_bar_scan, pct, LV_ANIM_OFF);
    } else {
        lv_obj_set_hidden(s_bar_scan, true);
    }
}

static void refresh_ap(void)
{
    netdash_settings_t cfg;
    settings_get(&cfg);
    lv_label_set_text(s_lbl_ap_ssid, wifi_mgr_get_ap_ssid());
    lv_label_set_text(s_lbl_ap_pass, cfg.ap_pass);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Drain anything the event handler or another task parked for us. */
    int want_page;
    bool toast_pending;
    char toast[sizeof(s_state.toast)];

    state_lock();
    want_page = s_state.want_page;
    s_state.want_page = -1;
    toast_pending = s_state.toast_pending;
    s_state.toast_pending = false;
    memcpy(toast, s_state.toast, sizeof(toast));
    state_unlock();

    int64_t now_us = esp_timer_get_time();

    if (want_page >= 0 && want_page < DISPLAY_PAGE_COUNT) {
        apply_page((display_page_t)want_page);
    }

    if (toast_pending) {
        lv_label_set_text(s_toast_lbl, toast);
        lv_obj_set_hidden(s_toast, false);
        s_toast_until_us = now_us + (int64_t)TOAST_MS * 1000;
    } else if (s_toast_until_us != 0 && now_us > s_toast_until_us) {
        lv_obj_set_hidden(s_toast, true);
        s_toast_until_us = 0;
    }

    switch (s_page) {
    case DISPLAY_PAGE_MAIN:
        refresh_main();
        break;
    case DISPLAY_PAGE_STATS:
        refresh_stats();
        break;
    case DISPLAY_PAGE_AP:
        refresh_ap();
        break;
    default:
        break;
    }

#if CONFIG_NETDASH_LCD_IDLE_DIM_SEC > 0
    if (!s_dimmed &&
        (now_us - s_last_activity_us) > ((int64_t)CONFIG_NETDASH_LCD_IDLE_DIM_SEC * 1000000)) {
        s_dimmed = true;
        backlight_set(BL_LEVEL_DIM);
        ESP_LOGD(TAG, "idle, dimming backlight");
    }
#endif
}

/* ------------------------------------------------------------------------- */
/* NETDASH_EVENT handler - no LVGL work here, only state for refresh_cb      */
/* ------------------------------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case NETDASH_EVENT_WIFI_AP_STARTED:
        state_lock();
        s_state.ap_up = true;
        s_state.want_page = DISPLAY_PAGE_AP;
        state_unlock();
        break;

    case NETDASH_EVENT_WIFI_AP_STOPPED:
        state_lock();
        s_state.ap_up = false;
        if (s_page == DISPLAY_PAGE_AP) {
            s_state.want_page = DISPLAY_PAGE_MAIN;
        }
        state_unlock();
        break;

    case NETDASH_EVENT_WIFI_STA_GOT_IP:
        state_lock();
        s_state.want_page = DISPLAY_PAGE_MAIN;
        state_unlock();
        break;

    case NETDASH_EVENT_DEVICE_NEW:
        if (data != NULL) {
            char name[32];
            device_db_display_name((const netdash_device_t *)data, name, sizeof(name));
            char text[sizeof(s_state.toast)];
            snprintf(text, sizeof(text), "New: %s", name);
            display_toast(text);
        }
        break;

    case NETDASH_EVENT_SCAN_STARTED:
        state_lock();
        s_state.scanning = true;
        s_state.scan_done = 0;
        s_state.scan_total = 0;
        state_unlock();
        break;

    case NETDASH_EVENT_SCAN_PROGRESS:
        if (data != NULL) {
            const netdash_scan_progress_t *p = (const netdash_scan_progress_t *)data;
            state_lock();
            s_state.scanning = true;
            s_state.scan_done = p->done;
            s_state.scan_total = p->total;
            state_unlock();
        }
        break;

    case NETDASH_EVENT_SCAN_DONE:
        state_lock();
        s_state.scanning = false;
        state_unlock();
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t display_init(void)
{
    s_state_mux = xSemaphoreCreateMutex();
    if (s_state_mux == NULL) {
        ESP_LOGE(TAG, "no memory for the state mutex, display disabled");
        return ESP_OK;
    }

    esp_err_t err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight: %s", esp_err_to_name(err));
    }

    /*
     * From here on a failure must not take the boot down with it: Wi-Fi and the
     * web UI have to keep working on a board whose panel is absent or wired
     * differently, so every error path just logs and returns ESP_OK.
     */
    err = panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD not available (%s) - continuing without it", esp_err_to_name(err));
        return ESP_OK;
    }

    err = lvgl_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL not available (%s) - continuing without it", esp_err_to_name(err));
        return ESP_OK;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "could not take the LVGL lock - continuing without the display");
        return ESP_OK;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    build_page_main();
    build_page_stats();
    build_page_ap();
    build_overlays();
    apply_page(DISPLAY_PAGE_MAIN);

    lv_timer_t *timer = lv_timer_create(refresh_cb, REFRESH_PERIOD_MS, NULL);
    lvgl_port_unlock();

    if (timer == NULL) {
        ESP_LOGE(TAG, "could not create the refresh timer - continuing without the display");
        return ESP_OK;
    }

    s_last_activity_us = esp_timer_get_time();
    s_ready = true;

    err = esp_event_handler_instance_register(NETDASH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event subscribe: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "LCD up: %dx%d, sclk=%d mosi=%d dc=%d rst=%d cs=%d bl=%d, gap %d/%d",
             LCD_H_RES, LCD_V_RES,
             CONFIG_NETDASH_PIN_LCD_SCLK, CONFIG_NETDASH_PIN_LCD_MOSI,
             CONFIG_NETDASH_PIN_LCD_DC, CONFIG_NETDASH_PIN_LCD_RST,
             CONFIG_NETDASH_PIN_LCD_CS, CONFIG_NETDASH_PIN_LCD_BL,
             CONFIG_NETDASH_LCD_X_OFFSET, CONFIG_NETDASH_LCD_Y_OFFSET);
    return ESP_OK;
}

bool display_is_ready(void)
{
    return s_ready;
}

esp_err_t display_show_page(display_page_t page)
{
    if (page >= DISPLAY_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        s_page = page;
        return ESP_OK;
    }
    display_wake();
    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    apply_page(page);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t display_next_page(void)
{
    bool ap_up;
    state_lock();
    ap_up = s_state.ap_up;
    state_unlock();

    display_page_t next = s_page;
    for (int step = 0; step < DISPLAY_PAGE_COUNT; step++) {
        next = (display_page_t)(((int)next + 1) % DISPLAY_PAGE_COUNT);
        if (next != DISPLAY_PAGE_AP || ap_up) {
            break;
        }
    }
    return display_show_page(next);
}

void display_wake(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        backlight_set(BL_LEVEL_FULL);
    }
}

void display_hold_countdown(int secs_left)
{
    if (!s_ready) {
        return;
    }
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (secs_left > 0) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", secs_left);
        lv_label_set_text(s_hold_lbl, buf);
        lv_obj_set_hidden(s_hold, false);
    } else {
        lv_obj_set_hidden(s_hold, true);
    }
    lvgl_port_unlock();
}

void display_toast(const char *text)
{
    if (text == NULL) {
        return;
    }
    if (!s_ready) {
        ESP_LOGI(TAG, "toast: %s", text);
        return;
    }
    state_lock();
    snprintf(s_state.toast, sizeof(s_state.toast), "%s", text);
    s_state.toast_pending = true;
    state_unlock();
}
