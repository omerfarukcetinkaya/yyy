/**
 * @file mq137.c
 * @brief MQ137 NH3/VOC gas sensor driver — ADC1 with Rs/R0 ppm conversion.
 *
 * MQ137 sensitivity curve (NH3, from datasheet Fig. 4):
 *   ppm = A * (Rs/R0)^B
 *   A = 102.7,  B = -2.149  (approximate — varies by lot; calibrate R0)
 *
 * The sensor also responds to other reducing gases (H2S, alcohol, benzene),
 * making it useful for general VOC/burning-cable detection.
 */
#include "mq137.h"
#include "adc_bus.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "mq137";

#define MQ137_RL_KOHM    10.0f
#define MQ137_R0_KOHM    10.0f   /* Calibrate in clean air */
#define MQ137_VC          3.3f
#define MQ137_A         102.7f
#define MQ137_B          (-2.149f)
#define MQ137_ADC_MAX   4095.0f
#define MQ137_VREF        3.1f
#define MQ137_OVERSAMPLE     4

static bool s_initialized = false;

esp_err_t mq137_init(void)
{
    esp_err_t ret = adc_bus_config_channel(BOARD_MQ137_ADC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "MQ137 initialized (ch=%d, R0=%.1f kOhm).",
             BOARD_MQ137_ADC_CHANNEL, MQ137_R0_KOHM);
    return ESP_OK;
}

esp_err_t mq137_read(float *nh3_ppm, float *raw_mv)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    int32_t sum = 0;
    for (int i = 0; i < MQ137_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_bus_read(BOARD_MQ137_ADC_CHANNEL, &raw) != ESP_OK) {
            return ESP_FAIL;
        }
        sum += raw;
    }
    float raw_avg = (float)sum / MQ137_OVERSAMPLE;
    float vout = raw_avg * MQ137_VREF / MQ137_ADC_MAX;
    if (raw_mv) *raw_mv = vout * 1000.0f;

    if (vout < 0.01f) {
        if (nh3_ppm) *nh3_ppm = 0.0f;
        return ESP_OK;
    }

    float rs    = ((MQ137_VC - vout) / vout) * MQ137_RL_KOHM;
    float ratio = rs / MQ137_R0_KOHM;
    float ppm   = MQ137_A * powf(ratio, MQ137_B);

    if (nh3_ppm) *nh3_ppm = (ppm < 0.0f) ? 0.0f : ppm;
    return ESP_OK;
}
