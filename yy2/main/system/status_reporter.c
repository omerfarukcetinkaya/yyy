#include "status_reporter.h"
#include "esp_log.h"

static const char *TAG = "status_rpt";

esp_err_t status_reporter_init(void)
{
    ESP_LOGI(TAG, "Status reporter init (STUB).");
    return ESP_OK;
}

esp_err_t status_reporter_start(void)
{
    ESP_LOGI(TAG, "Status reporter started (STUB).");
    return ESP_OK;
}
