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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_events.h"
#include "backup.h"
#include "button.h"
#include "device_db.h"
#include "disc_mdns.h"
#include "disc_nbns.h"
#include "disc_rdns.h"
#include "disc_ssdp.h"
#include "display.h"
#include "http_server.h"
#include "links.h"
#include "ota.h"
#include "icons.h"
#include "notes.h"
#include "notify.h"
#include "portscan.h"
#include "linkcheck.h"
#include "wan.h"
#include "scanner.h"
#include "settings.h"
#include "tls_mem.h"
#include "usb_restore.h"
#include "wifi_mgr.h"

static const char *TAG = "netdash";

ESP_EVENT_DEFINE_BASE(NETDASH_EVENT);

#define HEARTBEAT_PERIOD_US (30LL * 1000 * 1000)

/* Runs on the esp_timer task, so app_main can return and give back its stack. */
static void heartbeat(void *arg)
{
    (void)arg;
    tls_mem_stats_t tls;
    tls_mem_get_stats(&tls);
    ESP_LOGI(TAG, "heap free %" PRIu32 " min %" PRIu32 " largest %u devices %u "
                  "tls reserve %" PRIu32 "/%" PRIu32 " busy, %" PRIu32 " failed",
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)device_db_count(), tls.reserve_uses, tls.reserve_busy,
             tls.failures);
}

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
    ESP_ERROR_CHECK(tls_mem_init());

    init_nvs();
    /* Before anything reads NVS or mounts storage: see backup.h. */
    backup_apply_pending();
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
    ESP_ERROR_CHECK(ota_init());
    /* Last: a restore borrows the update slot. Not fatal - the dashboard can
       restore just as well. */
    if (usb_restore_init() != ESP_OK) {
        ESP_LOGW(TAG, "restore over USB unavailable");
    }

    const esp_timer_create_args_t hb = {.callback = heartbeat, .name = "heartbeat"};
    esp_timer_handle_t             hb_timer;
    ESP_ERROR_CHECK(esp_timer_create(&hb, &hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(hb_timer, HEARTBEAT_PERIOD_US));

    /*
     * Returning ends the main task and frees its 8 KB stack, which startup
     * needs (the backup restore runs here) and nothing after it does. Memory
     * is tight enough that 8 KB decides whether an update can download.
     */
    ESP_LOGI(TAG, "boot complete, free heap %" PRIu32 " bytes", esp_get_free_heap_size());
}
