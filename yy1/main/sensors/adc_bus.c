/**
 * @file adc_bus.c
 * @brief Shared ADC1 oneshot unit singleton.
 */
#include "adc_bus.h"
#include "esp_log.h"

static const char *TAG = "adc_bus";
static adc_oneshot_unit_handle_t s_unit = NULL;

esp_err_t adc_bus_init(void)
{
    if (s_unit != NULL) return ESP_OK;

    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&cfg, &s_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ADC1 oneshot unit ready.");
    return ESP_OK;
}

esp_err_t adc_bus_config_channel(adc_channel_t ch)
{
    if (!s_unit) return ESP_ERR_INVALID_STATE;

    adc_oneshot_chan_cfg_t cfg = {
        .atten    = ADC_ATTEN_DB_12,    /* 0-3.1 V input range */
        .bitwidth = ADC_BITWIDTH_12,
    };
    return adc_oneshot_config_channel(s_unit, ch, &cfg);
}

esp_err_t adc_bus_read(adc_channel_t ch, int *raw)
{
    if (!s_unit || !raw) return ESP_ERR_INVALID_STATE;
    return adc_oneshot_read(s_unit, ch, raw);
}
