/*
 * The USB end of a restore. See usb_restore.h for the protocol.
 */
#include "usb_restore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "backup.h"

static const char *TAG = "usb_restore";

#define CMD            "LANDASH-RESTORE "
#define CMD_LINE_MAX       48
#define RX_BUF         4096     /* the host keeps within 3072 unacknowledged */
#define ACK_EVERY      1024
#define TX_BUF         1024
#define DRAIN_QUIET_MS 300      /* nothing for this long: the queue is empty  */
#define RECV_WAIT_MS   10000    /* the host stalling this long is a failure   */
#define TASK_STACK     6144

typedef struct {
    size_t left;       /* bytes of the announced body still to come */
    size_t taken;      /* bytes read so far                         */
    size_t acked;      /* ...as of the last acknowledgement         */
} serial_rx_t;

/* One line of ours, on a line of its own whatever the log was halfway through. */
static void send_line(const char *text)
{
    char buf[400];
    const int n = snprintf(buf, sizeof(buf), "\nLANDASH:%s\n", text);
    if (n > 0) {
        usb_serial_jtag_write_bytes(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1,
                                    pdMS_TO_TICKS(50));
    }
}

/* Throws input away until none has arrived for quiet_ms, or max bytes are gone. */
static void drain(uint32_t quiet_ms, size_t max)
{
    uint8_t buf[64];
    while (max > 0) {
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf) < max ? sizeof(buf) : max,
                                                 pdMS_TO_TICKS(quiet_ms));
        if (n <= 0) {
            return;
        }
        max -= (size_t)n;
    }
}

static int serial_recv(void *ctx, void *buf, size_t len)
{
    serial_rx_t *rx = ctx;
    if (rx->left == 0) {
        return 0;
    }
    const size_t want = len < rx->left ? len : rx->left;
    const int    n    = usb_serial_jtag_read_bytes(buf, want, pdMS_TO_TICKS(RECV_WAIT_MS));
    if (n <= 0) {
        return -1;   /* the host went quiet part-way */
    }
    rx->left -= (size_t)n;
    rx->taken += (size_t)n;
    if (rx->taken - rx->acked >= ACK_EVERY || rx->left == 0) {
        char text[24];
        snprintf(text, sizeof(text), "ACK %u", (unsigned)rx->taken);
        send_line(text);
        rx->acked = rx->taken;
    }
    return n;
}

static void run_restore(size_t bytes)
{
    /* The installer repeats the command until it hears back, so copies of it
       may be queued behind the first. They must not be read as a passphrase. */
    drain(DRAIN_QUIET_MS, SIZE_MAX);
    send_line("READY");
    ESP_LOGI(TAG, "receiving a %u-byte restore over USB", (unsigned)bytes);

    serial_rx_t     rx = {.left = bytes};
    backup_info_t   info;
    char            msg[128];
    const esp_err_t err = backup_restore(serial_recv, &rx, &info, msg, sizeof(msg));

    /* A refusal can come before the whole body has arrived; take the rest, so
       the host's writes finish and it is listening when the answer comes. */
    drain(1000, rx.left);

    if (err != ESP_OK) {
        char line[200];
        /* "403 Forbidden" -> "403" */
        snprintf(line, sizeof(line), "ERR %.3s %s", backup_restore_status(err), msg);
        send_line(line);
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "restarting", true);
    cJSON_AddStringToObject(o, "hostname", info.hostname);
    cJSON_AddStringToObject(o, "fw", info.fw);
    cJSON_AddNumberToObject(o, "created", (double)info.created);
    cJSON_AddNumberToObject(o, "devices", info.devices);
    cJSON_AddNumberToObject(o, "links", info.links);
    cJSON_AddNumberToObject(o, "icons", info.icons);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    char line[360];
    snprintf(line, sizeof(line), "OK %s", json != NULL ? json : "{\"ok\":true}");
    cJSON_free(json);
    send_line(line);

    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void handle_line(const char *line)
{
    if (strncmp(line, CMD, strlen(CMD)) != 0) {
        return;   /* not for us */
    }
    char               *end   = NULL;
    const unsigned long bytes = strtoul(line + strlen(CMD), &end, 10);
    if (end == line + strlen(CMD) || *end != '\0' || bytes == 0) {
        send_line("ERR 400 The restore command was malformed.");
        return;
    }
    if (bytes > BACKUP_MAX_BYTES + BACKUP_PASS_MAX + 1) {
        send_line("ERR 413 The backup is too large to restore.");
        return;
    }
    run_restore((size_t)bytes);
}

static void rx_task(void *arg)
{
    (void)arg;
    char   line[CMD_LINE_MAX];
    size_t n        = 0;
    bool   overlong = false;

    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!overlong && n > 0) {
                line[n] = '\0';
                handle_line(line);
            }
            n        = 0;
            overlong = false;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        } else {
            overlong = true;   /* not a command of ours; skip to the next line */
        }
    }
}

static void send_ip(const esp_ip4_addr_t *ip)
{
    char text[32];
    snprintf(text, sizeof(text), "IP " IPSTR, IP2STR(ip));
    send_line(text);
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *ev = data;
    send_ip(&ev->ip_info.ip);
}

esp_err_t usb_restore_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size                  = RX_BUF;
    cfg.tx_buffer_size                  = TX_BUF;

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    /* From here the console goes through the driver too: two writers on the
       hardware FIFO would garble each other. */
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(rx_task, "usb_restore", TASK_STACK, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IP announcements: %s", esp_err_to_name(err));
    }

    /* Wi-Fi may have connected before this ran. */
    esp_netif_t        *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta != NULL && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        send_ip(&ip.ip);
    }

    ESP_LOGI(TAG, "listening for a restore over USB");
    return ESP_OK;
}
