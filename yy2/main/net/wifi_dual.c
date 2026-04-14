/**
 * @file wifi_dual.c
 * @brief Dual-band WiFi manager for ESP32-C5.
 *
 * Two SSIDs:
 *   2.4 GHz "localhost-ofc-dev0" — local IoT mesh (S3 camera, sensors)
 *   5 GHz   "Pr.Kothsv."        — internet (Telegram, VPN)
 *
 * Idle behavior: alternate 50/50 between bands (~30s each).
 * On task (alarm relay, Telegram, VPN): switch to required band,
 * hold until task completes, then resume alternation.
 */
#include "wifi_dual.h"
#include "status_reporter.h"
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_dual";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
/* Timing: ~90% on 2.4G (IoT/admin panel), ~10% on 5G (Telegram poll).
 * Cycle: 270s on 2.4G → switch to 5G → 30s on 5G → back to 2.4G.
 * During 5G window: Telegram polls, SNTP syncs, alerts send.
 * During 2.4G window: S3 telemetry polling, admin panel served. */
#define STAY_24G_MS     300000  /* 5 min on 2.4G — admin panel primary window */
#define STAY_5G_MS       20000  /* 20s on 5G — aggressive Telegram poll burst */

typedef enum {
    BAND_24G = 0,
    BAND_5G  = 1,
} band_t;

static struct {
    esp_netif_t        *netif;
    EventGroupHandle_t  events;
    band_t              current_band;
    bool                connected;
    bool                task_lock;       /* true = a task is holding the band */
    char                ip[16];
    int8_t              rssi;
    bool                got_first_ip;    /* true after first DHCP */
} s_wifi;

bool wifi_dual_got_first_ip(void) { return s_wifi.got_first_ip; }

/* ── Event handler ─────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            /* Don't auto-connect here — connect_to_band() handles it */
            ESP_LOGI(TAG, "STA started.");
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_wifi.connected = false;
            ESP_LOGW(TAG, "Disconnected from %s band.",
                     s_wifi.current_band == BAND_5G ? "5G" : "2.4G");
            /* Signal disconnect so connect_to_band can unblock */
            xEventGroupSetBits(s_wifi.events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_wifi.ip, sizeof(s_wifi.ip), IPSTR,
                 IP2STR(&event->ip_info.ip));
        s_wifi.connected = true;
        ESP_LOGI(TAG, "Connected [%s] IP: %s",
                 s_wifi.current_band == BAND_5G ? "5G" : "2.4G", s_wifi.ip);
        s_wifi.got_first_ip = true;
        xEventGroupSetBits(s_wifi.events, WIFI_CONNECTED_BIT);
        /* On every 2.4G IP acquisition, (re)start the admin HTTP server.
         * Idempotent — no-op if already running. This keeps the server
         * bound to the latest netif after band switch flaps. */
        if (s_wifi.current_band == BAND_24G) {
            status_reporter_ensure_http_server();
        }
    }
}

/* ── Internal connect ──────────────────────────────────────────────────── */

static esp_err_t connect_to_band(band_t band)
{
    /* Disconnect cleanly and wait for DISCONNECTED event before reconfiguring */
    if (s_wifi.connected) {
        esp_wifi_disconnect();
        /* Brief wait for disconnect event to process */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    xEventGroupClearBits(s_wifi.events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_wifi.connected = false;

    wifi_config_t cfg = {0};
    if (band == BAND_24G) {
        strncpy((char *)cfg.sta.ssid, CONFIG_SCOUT_WIFI_24G_SSID,
                sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, CONFIG_SCOUT_WIFI_24G_PASSWORD,
                sizeof(cfg.sta.password) - 1);
    } else {
        strncpy((char *)cfg.sta.ssid, CONFIG_SCOUT_WIFI_5G_SSID,
                sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, CONFIG_SCOUT_WIFI_5G_PASSWORD,
                sizeof(cfg.sta.password) - 1);
    }

    ESP_LOGI(TAG, "Connecting to %s ('%s')...",
             band == BAND_5G ? "5G" : "2.4G",
             band == BAND_5G ? CONFIG_SCOUT_WIFI_5G_SSID : CONFIG_SCOUT_WIFI_24G_SSID);

    s_wifi.current_band = band;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config failed: %s — retrying after 500ms", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_config retry failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    esp_wifi_connect();

    /* Wait up to 15s for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi.events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Connection timeout on %s band.",
             band == BAND_5G ? "5G" : "2.4G");
    return ESP_ERR_TIMEOUT;
}

/* ── Band alternation task ─────────────────────────────────────────────── */

static void band_switch_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000)); /* let system settle */

    while (true) {
        /* ── 2.4G phase: actively enforce 2.4G for STAY_24G_MS ─────── */
        int64_t phase_start = esp_timer_get_time();
        while ((esp_timer_get_time() - phase_start) < (int64_t)STAY_24G_MS * 1000LL) {
            if (s_wifi.task_lock) {
                /* A task temporarily needs a specific band — yield. */
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            /* If something moved us off 2.4G while unlocked, pull back. */
            if (s_wifi.current_band != BAND_24G) {
                ESP_LOGI(TAG, "Band-switch: reclaiming 2.4G");
                connect_to_band(BAND_24G);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* ── 5G phase: briefly visit 5G for Telegram poll / NTP ────── */
        if (!s_wifi.task_lock && s_wifi.current_band != BAND_5G) {
            ESP_LOGI(TAG, "Band-switch: scheduled 5G window");
            connect_to_band(BAND_5G);
        }
        vTaskDelay(pdMS_TO_TICKS(STAY_5G_MS));
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t wifi_dual_init(void)
{
    memset(&s_wifi, 0, sizeof(s_wifi));
    s_wifi.events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi.netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Start on 2.4G (IoT mesh) */
    esp_err_t ret = connect_to_band(BAND_24G);

    /* Background band alternation */
    xTaskCreate(band_switch_task, "band_sw", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "Dual-band WiFi initialized. 2.4G=%ds 5G=%ds cycle.",
             (int)(STAY_24G_MS / 1000), (int)(STAY_5G_MS / 1000));
    return ret;
}

esp_err_t wifi_dual_switch_to_5g(void)
{
    s_wifi.task_lock = true;
    if (s_wifi.current_band == BAND_5G && s_wifi.connected) return ESP_OK;
    return connect_to_band(BAND_5G);
}

esp_err_t wifi_dual_switch_to_24g(void)
{
    s_wifi.task_lock = true;
    if (s_wifi.current_band == BAND_24G && s_wifi.connected) return ESP_OK;
    return connect_to_band(BAND_24G);
}

void wifi_dual_release_lock(void)
{
    s_wifi.task_lock = false;
}

bool wifi_dual_is_connected(void) { return s_wifi.connected; }
bool wifi_dual_is_on_5g(void) { return s_wifi.current_band == BAND_5G; }

const char *wifi_dual_get_ip(void) { return s_wifi.ip; }
int8_t wifi_dual_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_wifi.rssi = ap.rssi;
    }
    return s_wifi.rssi;
}
