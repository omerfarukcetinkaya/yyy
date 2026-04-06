/**
 * @file watchdog.c
 * @brief Task Watchdog Timer (TWDT) integration.
 *
 * The TWDT is globally enabled via sdkconfig (ESP_TASK_WDT_EN=y,
 * timeout 10s, panic on trigger). This module provides per-task
 * subscribe/reset/unsubscribe helpers.
 */
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "wdt";

esp_err_t watchdog_init(void)
{
    /* TWDT is already initialized by IDF at boot via sdkconfig.
     * Nothing extra to configure here — subscriptions happen per-task. */
    ESP_LOGI(TAG, "TWDT initialized (10s timeout, panic on trigger).");
    return ESP_OK;
}

esp_err_t watchdog_subscribe_current_task(void)
{
    esp_err_t ret = esp_task_wdt_add(NULL); /* NULL = current task */
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Task '%s' subscribed to TWDT.", pcTaskGetName(NULL));
    } else if (ret == ESP_ERR_INVALID_ARG) {
        ESP_LOGD(TAG, "Task already subscribed to TWDT.");
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to subscribe task to TWDT: %s", esp_err_to_name(ret));
    }
    return ret;
}

void watchdog_reset(void)
{
    esp_task_wdt_reset();
}

void watchdog_unsubscribe_current_task(void)
{
    esp_err_t ret = esp_task_wdt_delete(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to unsubscribe from TWDT: %s", esp_err_to_name(ret));
    }
}
