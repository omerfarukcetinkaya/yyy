/**
 * @file mq7.c
 * @brief MQ7 CO sensor driver — ADC1 with Rs/R0 ppm conversion.
 *
 * The MQ7 uses a resistive sensing element. Output voltage rises as CO
 * concentration increases (resistance drops). Conversion:
 *   Vout  = raw * V_REF / ADC_MAX
 *   Rs    = ((Vc - Vout) / Vout) * RL    (voltage divider, RL=10kOhm)
 *   ppm   = A * (Rs/R0)^B               (from datasheet sensitivity curve)
 *
 * Default R0 = 10 kOhm (calibrate in clean air for accurate readings).
 * MQ7 needs ~3 min warm-up; readings during warm-up are elevated.
 *
 * Heating cycle (for full accuracy) requires PWM on a gate transistor:
 * 5V for 60s then 1.4V for 90s. Simplified here: continuous 5V mode.
 */
#include "mq7.h"
#include "adc_bus.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "mq7";

/* Sensor parameters (adjust R0 after calibration in clean air) */
#define MQ7_RL_KOHM     10.0f   /* Load resistor in kOhm */
#define MQ7_R0_KOHM     10.0f   /* Sensor resistance in clean air (calibrate) */
#define MQ7_VC           3.3f   /* Circuit supply voltage */
#define MQ7_A           98.38f  /* CO sensitivity curve coefficient */
#define MQ7_B           (-1.544f)
#define MQ7_ADC_MAX     4095.0f
#define MQ7_VREF         3.1f   /* ADC full-scale (DB_12 attenuation) */
#define MQ7_OVERSAMPLE      4   /* Average N samples to reduce noise */

static bool s_initialized = false;

esp_err_t mq7_init(void)
{
    esp_err_t ret = adc_bus_config_channel(BOARD_MQ7_ADC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "MQ7 initialized (ch=%d, R0=%.1f kOhm).",
             BOARD_MQ7_ADC_CHANNEL, MQ7_R0_KOHM);
    return ESP_OK;
}

esp_err_t mq7_read(float *co_ppm, float *raw_mv)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Oversample to reduce ADC noise */
    int32_t sum = 0;
    for (int i = 0; i < MQ7_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_bus_read(BOARD_MQ7_ADC_CHANNEL, &raw) != ESP_OK) {
            return ESP_FAIL;
        }
        sum += raw;
    }
    float raw_avg = (float)sum / MQ7_OVERSAMPLE;

    float vout = raw_avg * MQ7_VREF / MQ7_ADC_MAX;
    if (raw_mv) *raw_mv = vout * 1000.0f;

    /* Avoid division by zero near ADC floor */
    if (vout < 0.01f) {
        if (co_ppm) *co_ppm = 0.0f;
        return ESP_OK;
    }

    float rs   = ((MQ7_VC - vout) / vout) * MQ7_RL_KOHM;
    float ratio = rs / MQ7_R0_KOHM;
    float ppm  = MQ7_A * powf(ratio, MQ7_B);

    if (co_ppm) *co_ppm = (ppm < 0.0f) ? 0.0f : ppm;
    return ESP_OK;
}
