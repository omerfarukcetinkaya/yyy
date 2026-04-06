/**
 * @file i2c_bus.c
 * @brief Shared I2C master bus singleton.
 */
#include "i2c_bus.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus != NULL) return ESP_OK;

    i2c_master_bus_config_t cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = BOARD_SENSOR_I2C_PORT,
        .scl_io_num        = BOARD_SENSOR_SCL_GPIO,
        .sda_io_num        = BOARD_SENSOR_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus ready (SDA=%d SCL=%d %d Hz).",
             BOARD_SENSOR_SDA_GPIO, BOARD_SENSOR_SCL_GPIO,
             BOARD_SENSOR_I2C_FREQ);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus;
}

esp_err_t i2c_bus_recover(void)
{
    if (!s_bus) {
        ESP_LOGW(TAG, "i2c_bus_recover: bus not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    /* i2c_master_bus_reset() clocks SCL 9 times to release any slave holding
     * SDA low after an interrupted transaction, then sends a STOP condition.
     * All existing device handles on this bus remain valid afterwards. */
    esp_err_t ret = i2c_master_bus_reset(s_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus reset failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "I2C bus reset OK (9 SCL pulses + STOP).");
    }
    return ret;
}
