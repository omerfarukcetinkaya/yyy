/**
 * @file espnow_bridge.c
 * @brief ESP-NOW listener for local ESP32 mesh (receive alarms from S3).
 * STUB: will register ESP-NOW receive callback.
 */
#include "espnow_bridge.h"
#include "esp_log.h"

static const char *TAG = "espnow";

esp_err_t espnow_bridge_init(void)
{
    ESP_LOGI(TAG, "ESP-NOW bridge init (STUB).");
    return ESP_OK;
}
