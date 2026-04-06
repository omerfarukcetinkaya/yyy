/**
 * @file fault_handler.c
 * @brief Boot-time fault and reset-reason diagnostics.
 */
#include "fault_handler.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"

static const char *TAG = "fault";
static char s_reset_reason[48] = "unknown";

esp_err_t fault_handler_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    const char *reason_str;
    switch (reason) {
    case ESP_RST_POWERON:   reason_str = "power-on";          break;
    case ESP_RST_EXT:       reason_str = "external-reset";    break;
    case ESP_RST_SW:        reason_str = "software-reset";    break;
    case ESP_RST_PANIC:     reason_str = "PANIC/crash";       break;
    case ESP_RST_INT_WDT:   reason_str = "interrupt-wdt";     break;
    case ESP_RST_TASK_WDT:  reason_str = "task-wdt";          break;
    case ESP_RST_WDT:       reason_str = "other-wdt";         break;
    case ESP_RST_DEEPSLEEP: reason_str = "deep-sleep-wakeup"; break;
    case ESP_RST_BROWNOUT:  reason_str = "brownout";          break;
    case ESP_RST_SDIO:      reason_str = "SDIO";              break;
    default:                reason_str = "unknown";            break;
    }

    snprintf(s_reset_reason, sizeof(s_reset_reason), "%s", reason_str);

    if (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT) {
        ESP_LOGW(TAG, "!! Previous reset reason: %s !!", s_reset_reason);
    } else {
        ESP_LOGI(TAG, "Reset reason: %s", s_reset_reason);
    }

    return ESP_OK;
}

const char *fault_handler_reset_reason_str(void)
{
    return s_reset_reason;
}
