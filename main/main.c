/*
 * NetDash: home network dashboard for the Waveshare ESP32-C6-GEEK.
 *
 * Boot order matters: NVS and the default event loop first (everything else
 * posts events), then netif, then settings (wifi_mgr and scanner read them),
 * then the display so the user sees something during the Wi-Fi connect, then
 * the network stack, the database, the web server and finally the scanner.
 */
#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_events.h"
#include "button.h"
#include "device_db.h"
#include "disc_mdns.h"
#include "disc_nbns.h"
#include "disc_rdns.h"
#include "disc_ssdp.h"
#include "display.h"
#include "http_server.h"
#include "links.h"
#include "icons.h"
#include "notes.h"
#include "notify.h"
#include "portscan.h"
#include "linkcheck.h"
#include "wan.h"
#include "scanner.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "netdash";

ESP_EVENT_DEFINE_BASE(NETDASH_EVENT);

#define HEARTBEAT_PERIOD_MS 30000

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs an erase (%s), reformatting", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "NetDash %s (IDF %s) starting", app->version, app->idf_ver);

    init_nvs();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(notify_init());
    ESP_ERROR_CHECK(notes_init());
    ESP_ERROR_CHECK(icons_init());
    ESP_ERROR_CHECK(device_db_init());
    ESP_ERROR_CHECK(links_init());
    ESP_ERROR_CHECK(http_server_init());
    ESP_ERROR_CHECK(scanner_start());

    /* All four share one task; each init after the first is a no-op. */
    ESP_ERROR_CHECK(disc_mdns_init());
    ESP_ERROR_CHECK(disc_ssdp_init());
    ESP_ERROR_CHECK(disc_rdns_init());
    ESP_ERROR_CHECK(disc_nbns_init());
    ESP_ERROR_CHECK(portscan_init());
    ESP_ERROR_CHECK(wan_init());
    ESP_ERROR_CHECK(linkcheck_init());

    ESP_LOGI(TAG, "boot complete, free heap %" PRIu32 " bytes", esp_get_free_heap_size());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
        ESP_LOGI(TAG, "heap free %" PRIu32 " min %" PRIu32 " devices %u",
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size(),
                 (unsigned)device_db_count());
    }
}
