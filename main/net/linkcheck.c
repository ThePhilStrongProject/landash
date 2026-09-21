/*
 * Per-link service checks. See linkcheck.h for why the device dot was not
 * enough.
 *
 * The probe is the same non-blocking connect the port scanner uses: a
 * completed connect means the port is open, ECONNREFUSED means it is closed
 * and comes back immediately, silence until the timeout means filtered. One
 * socket at a time, because there are at most a few dozen links and no hurry.
 */
#include "linkcheck.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <fcntl.h>
#include <unistd.h>

#include "device_db.h"
#include "links.h"
#include "wifi_mgr.h"

static const char *TAG = "linkcheck";

#define LC_TASK_STACK 3584
#define LC_TASK_PRIO  3

/* Breathing room between probes so a dashboard full of links is still a
   trickle rather than a burst. */
#define LC_GAP_MS     250

/* Idle poll when there is nothing to check. */
#define LC_IDLE_MS    2000

typedef struct {
    uint16_t id;
    uint8_t  state;     /* netdash_svc_state_t */
    int64_t  checked;   /* unix seconds, 0 = never */
} lc_rec_t;

static lc_rec_t          s_recs[NETDASH_MAX_LINKS];
static size_t            s_count;
static SemaphoreHandle_t s_lock;
static volatile bool     s_now;

/* ------------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* Caller holds the lock. */
static lc_rec_t *find_locked(uint16_t id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_recs[i].id == id) {
            return &s_recs[i];
        }
    }
    return NULL;
}

static void record(uint16_t id, netdash_svc_state_t state, int64_t when)
{
    lock();
    lc_rec_t *r = find_locked(id);
    if (r == NULL && s_count < NETDASH_MAX_LINKS) {
        r     = &s_recs[s_count++];
        r->id = id;
    }
    if (r != NULL) {
        r->state   = (uint8_t)state;
        r->checked = when;
    }
    unlock();
}

/*
 * One connect to ip:port. Returns UP when it completes, DOWN when it is
 * refused or stays silent. Modelled on portscan's probe, minus the batching:
 * there is no rush here and a single socket keeps well clear of the lwIP
 * socket budget the web server is also drawing on.
 */
static netdash_svc_state_t probe(uint32_t ip, uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return NETDASH_SVC_UNKNOWN;   /* our problem, not the service's */
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return NETDASH_SVC_UNKNOWN;
    }

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(ip),
    };

    netdash_svc_state_t out = NETDASH_SVC_DOWN;
    const int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));

    if (rc == 0) {
        out = NETDASH_SVC_UP;
    } else if (errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);

        struct timeval tv = {
            .tv_sec  = NETDASH_LINKCHECK_TIMEOUT_MS / 1000,
            .tv_usec = (NETDASH_LINKCHECK_TIMEOUT_MS % 1000) * 1000,
        };
        if (select(fd + 1, NULL, &wset, NULL, &tv) > 0 && FD_ISSET(fd, &wset)) {
            /* Writable only means the connect finished; SO_ERROR says whether
               it succeeded. Without this a refused port looks open. */
            int       err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                out = NETDASH_SVC_UP;
            }
        }
    }

    close(fd);
    return out;
}

static void linkcheck_task(void *arg)
{
    (void)arg;

    /* Let discovery settle so the first pass has real addresses to aim at. */
    vTaskDelay(pdMS_TO_TICKS(20000));

    int64_t next_pass_us = 0;

    for (;;) {
        const wifi_mgr_mode_t mode = wifi_mgr_get_mode();
        if (mode != WIFI_MGR_MODE_STA && mode != WIFI_MGR_MODE_APSTA) {
            vTaskDelay(pdMS_TO_TICKS(LC_IDLE_MS));
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        if (!s_now && now_us < next_pass_us) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        s_now = false;

        const size_t n = links_count();
        size_t       probed = 0, up = 0;

        for (size_t i = 0; i < n; i++) {
            netdash_link_t l;
            if (!links_get_at(i, &l)) {
                break;
            }

            netdash_device_t dev;
            const bool known = device_db_get_by_mac(l.mac, &dev);

            /*
             * A link to a device the sweep says is offline is left UNKNOWN
             * rather than probed: waiting out a timeout to confirm what is
             * already known would be the slowest part of the pass, and the
             * dashboard shows "offline" for that case anyway.
             */
            if (!known || !netdash_device_online(&dev) || dev.ip == 0) {
                record(l.id, NETDASH_SVC_UNKNOWN, 0);
                continue;
            }

            const netdash_svc_state_t st = probe(dev.ip, l.port);
            record(l.id, st, (int64_t)time(NULL));
            probed++;
            if (st == NETDASH_SVC_UP) {
                up++;
            }

            vTaskDelay(pdMS_TO_TICKS(LC_GAP_MS));
        }

        if (probed > 0) {
            ESP_LOGD(TAG, "checked %u link(s), %u answering", (unsigned)probed, (unsigned)up);
        }
        next_pass_us = esp_timer_get_time() +
                       (int64_t)NETDASH_LINKCHECK_PERIOD_S * 1000000;
    }
}

/* ------------------------------------------------------------------------- */

esp_err_t linkcheck_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(linkcheck_task, "linkcheck", LC_TASK_STACK, NULL, LC_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "failed to start the checker task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

netdash_svc_state_t linkcheck_get(uint16_t link_id, int64_t *out_checked)
{
    netdash_svc_state_t state = NETDASH_SVC_UNKNOWN;
    int64_t             when  = 0;

    lock();
    const lc_rec_t *r = find_locked(link_id);
    if (r != NULL) {
        state = (netdash_svc_state_t)r->state;
        when  = r->checked;
    }
    unlock();

    if (out_checked != NULL) {
        *out_checked = when;
    }
    return state;
}

void linkcheck_forget(uint16_t link_id)
{
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (s_recs[i].id == link_id) {
            memmove(&s_recs[i], &s_recs[i + 1], sizeof(s_recs[0]) * (s_count - i - 1));
            s_count--;
            break;
        }
    }
    unlock();
}

void linkcheck_now(void)
{
    s_now = true;
}

const char *netdash_svc_state_name(netdash_svc_state_t state)
{
    switch (state) {
    case NETDASH_SVC_UP:   return "up";
    case NETDASH_SVC_DOWN: return "down";
    default:               return "unknown";
    }
}
