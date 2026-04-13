#include "watchdog.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

esp_err_t watchdog_init(void) { return ESP_OK; }
void watchdog_reset(void) { esp_task_wdt_reset(); }
