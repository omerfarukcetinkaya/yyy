/**
 * @file watchdog.c
 * @brief Task watchdog configuration for Scout.
 *
 * Only explicitly-subscribed tasks reset the WDT. Scout's watchdog
 * strategy: subscribe only the status_reporter task (which resets
 * every 2 seconds). Other tasks (network / polling) are not watched
 * because their timing is dependent on external services (WiFi,
 * Telegram, S3) and would produce false positives.
 *
 * IDLE task watchdog is OFF (default in sdkconfig.defaults) — C5 is
 * single-core and idle may be starved briefly during busy operations
 * without implying a hang.
 */
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "wdt";

esp_err_t watchdog_init(void)
{
    /* Reconfigure TWDT: 15s timeout, NO idle task subscription, no panic.
     * panic=false so crash-loop protection (reboot counter in NVS) can
     * handle runaway crashes instead of rebooting blindly. */
    esp_task_wdt_config_t cfg = {
        .timeout_ms   = 15000,
        .trigger_panic = true,
        .idle_core_mask = 0,  /* no core idle monitoring */
    };

    /* esp_task_wdt_init fails if already init'd by auto-init (sdkconfig).
     * Use reconfigure if available, else accept existing config. */
    esp_err_t r = esp_task_wdt_reconfigure(&cfg);
    if (r == ESP_ERR_INVALID_STATE) {
        /* Not initialized yet — init now. */
        r = esp_task_wdt_init(&cfg);
    }
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "WDT configure returned %s", esp_err_to_name(r));
    }
    ESP_LOGI(TAG, "WDT configured: 15s timeout, no idle monitoring");
    return ESP_OK;
}

void watchdog_reset(void)
{
    /* Only succeeds if calling task was subscribed. Safe to call from
     * unsubscribed tasks — returns an error we silently ignore. */
    (void)esp_task_wdt_reset();
}

esp_err_t watchdog_subscribe_current_task(void)
{
    esp_err_t r = esp_task_wdt_add(NULL);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "Task subscribed to WDT");
    } else if (r != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "WDT subscribe failed: %s", esp_err_to_name(r));
    }
    return r;
}
