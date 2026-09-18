/*
 * BOOT button (GPIO 9 on the Waveshare ESP32-C6-GEEK, confirmed against
 * ESP32-C6-GEEK-Demo.zip ESP-IDF/04_button/main/Button_Driver/button_driver.h
 * line 4: "#define BOOT_BUTTON_NUM 9"). Active low with the internal pull-up,
 * which is what iot_button configures for active_level 0.
 *
 * Short press  : next display page, and wake the backlight.
 * 5 s hold     : wipe the stored Wi-Fi credentials and reboot. The last three
 *                seconds of the hold show a 3 / 2 / 1 countdown on the LCD, and
 *                releasing the button at any point before 5 s cancels it.
 *
 * iot_button callbacks run in its own timer task, so the NVS write and the
 * restart are handed to a short-lived task of their own rather than being done
 * on that stack.
 */
#include "button.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "button_gpio.h"
#include "iot_button.h"

#include "display.h"
#include "settings.h"

static const char *TAG = "button";

/* Hold milestones, in ms. The last one is the point of no return. */
#define HOLD_COUNTDOWN_3_MS 2000
#define HOLD_COUNTDOWN_2_MS 3000
#define HOLD_COUNTDOWN_1_MS 4000
#define HOLD_RESET_MS       5000

static button_handle_t s_btn;
static bool s_resetting;

static void reset_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "BOOT held for %d ms: clearing Wi-Fi credentials and rebooting",
             HOLD_RESET_MS);
    esp_err_t err = settings_clear_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings_clear_wifi: %s", esp_err_to_name(err));
    }
    display_toast("Wi-Fi cleared - rebooting");

    /* Long enough for the toast to be seen and the log to drain. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void on_click(void *handle, void *data)
{
    (void)handle;
    (void)data;

    esp_err_t err = display_next_page();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display_next_page: %s", esp_err_to_name(err));
    }
}

static void on_hold_tick(void *handle, void *data)
{
    (void)handle;
    display_hold_countdown((int)(intptr_t)data);
}

static void on_hold_reset(void *handle, void *data)
{
    (void)handle;
    (void)data;

    if (s_resetting) {
        return;
    }
    s_resetting = true;
    display_hold_countdown(0);

    if (xTaskCreate(reset_task, "netdash_reset", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the reset task, restarting anyway");
        esp_restart();
    }
}

static void on_release(void *handle, void *data)
{
    (void)handle;
    (void)data;

    if (!s_resetting) {
        display_hold_countdown(0);
    }
}

static esp_err_t register_hold_cb(uint16_t press_time_ms, button_cb_t cb, void *data)
{
    button_event_args_t args = {
        .long_press = { .press_time = press_time_ms },
    };
    return iot_button_register_cb(s_btn, BUTTON_LONG_PRESS_START, &args, cb, data);
}

esp_err_t button_init(void)
{
#if CONFIG_NETDASH_PIN_BUTTON < 0
    ESP_LOGW(TAG, "no button GPIO configured");
    return ESP_OK;
#else
    const button_config_t btn_cfg = {
        /* Line up the first threshold with the first countdown step. */
        .long_press_time  = HOLD_COUNTDOWN_3_MS,
        .short_press_time = 0,      /* component default */
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num          = CONFIG_NETDASH_PIN_BUTTON,
        .active_level      = 0,     /* BOOT pulls the pin low */
        .enable_power_save = false,
        .disable_pull      = false, /* internal pull-up */
    };

    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_btn);
    if (err != ESP_OK) {
        /* A dead button must not stop the web UI from coming up. */
        ESP_LOGE(TAG, "iot_button_new_gpio_device(gpio %d): %s",
                 CONFIG_NETDASH_PIN_BUTTON, esp_err_to_name(err));
        return ESP_OK;
    }

    err = iot_button_register_cb(s_btn, BUTTON_SINGLE_CLICK, NULL, on_click, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "single click cb: %s", esp_err_to_name(err));
    }

    struct {
        uint16_t ms;
        int      shown;
    } steps[] = {
        { HOLD_COUNTDOWN_3_MS, 3 },
        { HOLD_COUNTDOWN_2_MS, 2 },
        { HOLD_COUNTDOWN_1_MS, 1 },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        err = register_hold_cb(steps[i].ms, on_hold_tick, (void *)(intptr_t)steps[i].shown);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hold cb %u ms: %s", (unsigned)steps[i].ms, esp_err_to_name(err));
        }
    }

    err = register_hold_cb(HOLD_RESET_MS, on_hold_reset, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset cb: %s", esp_err_to_name(err));
    }

    err = iot_button_register_cb(s_btn, BUTTON_PRESS_UP, NULL, on_release, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "release cb: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "BOOT button on GPIO %d: click = next page, %d s hold = Wi-Fi reset",
             CONFIG_NETDASH_PIN_BUTTON, HOLD_RESET_MS / 1000);
    return ESP_OK;
#endif
}
