/**
 * @file app_main.c
 * @brief Scout — ESP32-C5 network bridge and alarm relay.
 *
 * Roles:
 *   1. Dual-band WiFi gateway (2.4 GHz IoT mesh ↔ 5 GHz internet)
 *   2. Telegram alarm relay (poll S3 /api/telemetry for alarm state)
 *   3. Keyword command handler via Telegram SecBridge topic
 *   4. Telemetry HTTP server on 2.4G (port 8080)
 *   5. Multi-device status aggregator (future: ESP-NOW for mesh)
 *
 * Active schedule: 09:00 — 19:00 (configurable).
 * Outside schedule: alarms are suppressed.
 * /mute command: suppresses alarms until next schedule start.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include "net/wifi_dual.h"
#include "net/telegram_client.h"
#include "net/espnow_bridge.h"
#include "system/watchdog.h"
#include "system/status_reporter.h"
#include "system/rgb_led.h"
#include "system/sensor_registry.h"
#include "system/scout_health.h"

static const char *TAG = "scout";

/* ── SNTP time sync (needed for schedule + alarm timestamps) ──────────── */
static void init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void)
{
    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2b. Scout self health (before WiFi so reset reason + reboot counter
     * are loaded early from NVS and available for the first heartbeat). */
    ESP_ERROR_CHECK(scout_health_init());
    ESP_ERROR_CHECK(scout_health_start());

    /* 3. WiFi dual-band (starts on 2.4G, alternates with 5G) */
    ESP_ERROR_CHECK(wifi_dual_init());

    /* 4. SNTP time sync (runs after WiFi connects to 5G) */
    init_sntp();

    /* 5. Telegram client (polls commands, sends alerts) */
    ESP_ERROR_CHECK(telegram_client_init());

    /* 6a. Sensor registry — multi-sensor polling (S3 is sensor 0) */
    ESP_ERROR_CHECK(sensor_registry_init());
    ESP_ERROR_CHECK(sensor_registry_start());

    /* 6b. ESP-NOW bridge (BIST heartbeat + alarm aggregation) */
    ESP_ERROR_CHECK(espnow_bridge_init());

    /* 7. Status reporter (HTTP telemetry server + UART log) */
    ESP_ERROR_CHECK(status_reporter_init());
    ESP_ERROR_CHECK(status_reporter_start());

    /* 8. RGB LED (off by default, red on alarm) */
    rgb_led_init();  /* non-critical — continue if fails */

    /* 9. Watchdog */
    ESP_ERROR_CHECK(watchdog_init());

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Scout — ESP32-C5 Network Bridge");
    ESP_LOGI(TAG, "  2.4G: %s", CONFIG_SCOUT_WIFI_24G_SSID);
    ESP_LOGI(TAG, "  5G:   %s", CONFIG_SCOUT_WIFI_5G_SSID);
    ESP_LOGI(TAG, "  S3:   %s:%d", CONFIG_SCOUT_S3_IP, CONFIG_SCOUT_S3_HTTP_PORT);
    ESP_LOGI(TAG, "  TG:   SecBridge (thread %d)", CONFIG_SCOUT_TG_THREAD_ID);
    ESP_LOGI(TAG, "  Schedule: %02d:00 — %02d:00",
             CONFIG_SCOUT_SCHEDULE_START_HOUR, CONFIG_SCOUT_SCHEDULE_END_HOUR);
    ESP_LOGI(TAG, "  Telemetry: http://<2.4G_IP>:%d/api/status",
             CONFIG_SCOUT_TELEMETRY_PORT);
    ESP_LOGI(TAG, "========================================");

    /* Boot notification task — needs 8KB stack (telegram_send uses large buffers) */
    extern void boot_notify_task(void *arg);
    xTaskCreate(boot_notify_task, "boot_notify", 8192, NULL, 2, NULL);
}

#include <stdio.h>

void boot_notify_task(void *arg)
{
    /* Wait up to 20s for first IP on 2.4G */
    for (int i = 0; i < 40; i++) {
        if (wifi_dual_got_first_ip() && !wifi_dual_is_on_5g()) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    char msg[256];
    snprintf(msg, sizeof(msg),
        "🟢 <b>Scout Online</b>\\n"
        "ESP32-C5 bridge başlatıldı.\\n"
        "\\n"
        "🌐 <b>Local IP</b>: <code>%s</code>\\n"
        "📡 Admin panel: http://%s:%d/\\n"
        "🔑 User: %s / %s",
        wifi_dual_get_ip(), wifi_dual_get_ip(),
        CONFIG_SCOUT_TELEMETRY_PORT,
        CONFIG_SCOUT_S3_USER, CONFIG_SCOUT_S3_PASS);
    telegram_send(msg);
    vTaskDelete(NULL);
}
