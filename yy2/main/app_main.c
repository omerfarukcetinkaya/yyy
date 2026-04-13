/**
 * @file app_main.c
 * @brief Scout — ESP32-C5 network bridge and alarm relay.
 *
 * Roles:
 *   1. Dual-band WiFi gateway (2.4 GHz IoT mesh ↔ 5 GHz internet)
 *   2. Telegram alarm relay (receive alert from S3 via ESP-NOW, send to phone)
 *   3. VPN relay (forward S3 camera stream to remote client)
 *   4. Multi-device status aggregator
 *
 * Boot sequence:
 *   1. NVS init
 *   2. WiFi dual-band init (start on 2.4 GHz)
 *   3. ESP-NOW bridge init (listen for S3 alarm packets)
 *   4. Status reporter init
 *   5. Watchdog init
 *   6. Main loop: aggregate status, relay alerts, band-switch on demand
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"

#include "net/wifi_dual.h"
#include "net/telegram_client.h"
#include "net/espnow_bridge.h"
#include "system/watchdog.h"
#include "system/status_reporter.h"

static const char *TAG = "scout";

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

    /* 3. WiFi dual-band */
    ESP_ERROR_CHECK(wifi_dual_init());

    /* 4. ESP-NOW bridge */
    ESP_ERROR_CHECK(espnow_bridge_init());

    /* 5. Telegram client */
    ESP_ERROR_CHECK(telegram_client_init());

    /* 6. Status reporter */
    ESP_ERROR_CHECK(status_reporter_init());
    ESP_ERROR_CHECK(status_reporter_start());

    /* 7. Watchdog */
    ESP_ERROR_CHECK(watchdog_init());

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Scout — ESP32-C5 Network Bridge");
    ESP_LOGI(TAG, "  WiFi: dual-band 2.4G/5G");
    ESP_LOGI(TAG, "  Role: alarm relay + VPN gateway");
    ESP_LOGI(TAG, "========================================");
}
