/*
 * BOOT button: short press cycles the display pages and wakes the backlight,
 * a 5 s hold wipes the Wi-Fi credentials and reboots.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the iot_button callbacks on CONFIG_NETDASH_PIN_BUTTON.
 *
 * Like display_init(), this never fails the boot: a button that cannot be
 * claimed is logged at ERROR and ESP_OK is still returned, so the web UI stays
 * reachable on a board variant that wires BOOT differently.
 */
esp_err_t button_init(void);

#ifdef __cplusplus
}
#endif
