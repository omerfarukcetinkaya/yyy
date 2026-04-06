/**
 * @file aht20.c
 * @brief AHT20 temperature + humidity driver via shared I2C bus.
 *
 * Protocol:
 *   1. Check status (0x71): calibrated bit[3] must be 1; send init if not.
 *   2. Trigger measurement: 0xAC 0x33 0x00
 *   3. Wait 80 ms, poll busy bit[7] until clear.
 *   4. Read 6 bytes and parse humidity / temperature.
 */
#include "aht20.h"
#include "i2c_bus.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aht20";
static i2c_master_dev_handle_t s_dev = NULL;

#define AHT20_CMD_INIT  0xBE
#define AHT20_CMD_TRIG  0xAC

esp_err_t aht20_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Remove stale handle before re-adding — required for reinit after bus recovery.
     * i2c_master_bus_add_device() does not check for duplicates; calling it with
     * an old handle still registered leaves a leaked, potentially broken handle on
     * the bus that causes subsequent transmissions to fail. */
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_AHT20_I2C_ADDR,
        .scl_speed_hz    = BOARD_SENSOR_I2C_FREQ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(20));  /* Power-on stabilization */

    /* Read status byte; if calibration bit is missing, send init command */
    uint8_t status = 0;
    bool need_init = true;
    if (i2c_master_receive(s_dev, &status, 1, 100) == ESP_OK) {
        need_init = !(status & 0x08);
    }
    if (need_init) {
        uint8_t cmd[] = {AHT20_CMD_INIT, 0x08, 0x00};
        i2c_master_transmit(s_dev, cmd, sizeof(cmd), 100);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "AHT20 initialized (addr=0x%02X).", BOARD_AHT20_I2C_ADDR);
    return ESP_OK;
}

esp_err_t aht20_read(float *temp_c, float *humidity)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    uint8_t trig[] = {AHT20_CMD_TRIG, 0x33, 0x00};
    esp_err_t ret = i2c_master_transmit(s_dev, trig, sizeof(trig), 100);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(85));  /* Measurement time */

    uint8_t data[6] = {0};
    for (int i = 0; i < 3; i++) {
        ret = i2c_master_receive(s_dev, data, sizeof(data), 100);
        if (ret != ESP_OK) return ret;
        if (!(data[0] & 0x80)) break;   /* Busy bit cleared */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (data[0] & 0x80) return ESP_ERR_TIMEOUT;

    /* Humidity: bits [43:24] of response */
    uint32_t raw_hum  = ((uint32_t)data[1] << 12)
                      | ((uint32_t)data[2] <<  4)
                      | ((data[3] >>  4) & 0x0F);
    /* Temperature: bits [23:4] of response */
    uint32_t raw_temp = ((uint32_t)(data[3] & 0x0F) << 16)
                      | ((uint32_t)data[4] << 8)
                      | data[5];

    if (humidity) *humidity = (float)raw_hum  / (1u << 20) * 100.0f;
    if (temp_c)   *temp_c   = (float)raw_temp / (1u << 20) * 200.0f - 50.0f;

    return ESP_OK;
}
