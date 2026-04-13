/**
 * @file wifi_dual.c
 * @brief Dual-band WiFi manager — 2.4 GHz (IoT mesh) ↔ 5 GHz (internet).
 * STUB: scaffold for ESP32-C5 WiFi 6 dual-band.
 */
#include "wifi_dual.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "wifi_dual";

esp_err_t wifi_dual_init(void)
{
    ESP_LOGI(TAG, "WiFi dual-band init (STUB).");
    return ESP_OK;
}

esp_err_t wifi_dual_switch_to_5g(void)
{
    ESP_LOGI(TAG, "Switching to 5 GHz (STUB).");
    return ESP_OK;
}

esp_err_t wifi_dual_switch_to_24g(void)
{
    ESP_LOGI(TAG, "Switching to 2.4 GHz (STUB).");
    return ESP_OK;
}

bool wifi_dual_is_connected(void) { return false; }
bool wifi_dual_is_on_5g(void) { return false; }
