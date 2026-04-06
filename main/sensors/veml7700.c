/**
 * @file veml7700.c
 * @brief VEML7700 ambient light sensor via shared I2C bus.
 *
 * Configuration: ALS gain = 1x, integration time = 100ms.
 * Lux resolution factor = 0.0576 (from VEML7700 datasheet Table 1).
 *
 * Register layout (16-bit little-endian):
 *   0x00 = ALS_CONF (config)
 *   0x04 = ALS      (raw light)
 *   0x05 = WHITE    (broadband, not used here)
 */
#include "veml7700.h"
#include "i2c_bus.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "veml7700";
static i2c_master_dev_handle_t s_dev = NULL;

/* Lux factor: gain=1x, IT=100ms → 0.0576 lux/count */
#define VEML7700_LUX_FACTOR  0.0576f

static esp_err_t veml7700_write_reg(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, val & 0xFF, (val >> 8) & 0xFF};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t veml7700_read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t data[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, 2, 100);
    if (ret == ESP_OK) {
        *val = (uint16_t)(data[0] | (data[1] << 8));
    }
    return ret;
}

esp_err_t veml7700_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Remove stale handle before re-adding (see aht20.c for explanation). */
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_VEML7700_I2C_ADDR,
        .scl_speed_hz    = BOARD_SENSOR_I2C_FREQ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ALS_CONF: ALS_GAIN=00(1x), ALS_IT=00(100ms), ALS_PERS=00(1), ALS_INT_EN=0, ALS_SD=0 */
    ret = veml7700_write_reg(0x00, 0x0000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(110));  /* Wait first integration period */
    ESP_LOGI(TAG, "VEML7700 initialized (addr=0x%02X).", BOARD_VEML7700_I2C_ADDR);
    return ESP_OK;
}

esp_err_t veml7700_read(float *lux)
{
    if (!s_dev || !lux) return ESP_ERR_INVALID_STATE;

    uint16_t raw = 0;
    esp_err_t ret = veml7700_read_reg(0x04, &raw);
    if (ret != ESP_OK) return ret;

    *lux = (float)raw * VEML7700_LUX_FACTOR;
    return ESP_OK;
}
