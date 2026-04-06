/**
 * @file bmp280.c
 * @brief BMP280 barometric pressure + temperature driver via shared I2C bus.
 *
 * Reads 24-byte factory calibration, configures normal mode (1x temp oversampling,
 * 16x pressure oversampling, IIR filter coeff 2), then reads compensated values.
 * Compensation formulas are taken verbatim from the BMP280 datasheet Appendix.
 */
#include "bmp280.h"
#include "i2c_bus.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bmp280";

/* Register map */
#define BMP280_REG_CALIB_START  0x88
#define BMP280_REG_ID           0xD0
#define BMP280_REG_RESET        0xE0
#define BMP280_REG_CTRL_MEAS    0xF4
#define BMP280_REG_CONFIG       0xF5
#define BMP280_REG_DATA_START   0xF7    /* 6 bytes: press[2:0], temp[2:0] */
#define BMP280_CHIP_ID          0x60    /* BMP280 = 0x58, BME280 = 0x60 */

/* Calibration data (from 0x88..0x9F) */
typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
} bmp280_calib_t;

static i2c_master_dev_handle_t s_dev  = NULL;
static bmp280_calib_t          s_cal  = {0};
static int32_t                 s_tfine = 0;

static esp_err_t bmp280_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static esp_err_t bmp280_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 200);
}

static void load_calib(void)
{
    uint8_t raw[24] = {0};
    if (bmp280_read_regs(BMP280_REG_CALIB_START, raw, 24) != ESP_OK) return;

    s_cal.dig_T1 = (uint16_t)(raw[1]  << 8 | raw[0]);
    s_cal.dig_T2 = (int16_t) (raw[3]  << 8 | raw[2]);
    s_cal.dig_T3 = (int16_t) (raw[5]  << 8 | raw[4]);
    s_cal.dig_P1 = (uint16_t)(raw[7]  << 8 | raw[6]);
    s_cal.dig_P2 = (int16_t) (raw[9]  << 8 | raw[8]);
    s_cal.dig_P3 = (int16_t) (raw[11] << 8 | raw[10]);
    s_cal.dig_P4 = (int16_t) (raw[13] << 8 | raw[12]);
    s_cal.dig_P5 = (int16_t) (raw[15] << 8 | raw[14]);
    s_cal.dig_P6 = (int16_t) (raw[17] << 8 | raw[16]);
    s_cal.dig_P7 = (int16_t) (raw[19] << 8 | raw[18]);
    s_cal.dig_P8 = (int16_t) (raw[21] << 8 | raw[20]);
    s_cal.dig_P9 = (int16_t) (raw[23] << 8 | raw[22]);
}

/* Datasheet Appendix: 32-bit integer compensation (returns T in 0.01 degC) */
static int32_t compensate_T(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)s_cal.dig_T1 << 1)))
                    * (int32_t)s_cal.dig_T2) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)s_cal.dig_T1)
                    * ((adc_T >> 4) - (int32_t)s_cal.dig_T1)) >> 12)
                    * (int32_t)s_cal.dig_T3) >> 14;
    s_tfine = var1 + var2;
    return (s_tfine * 5 + 128) >> 8;
}

/* Datasheet Appendix: 64-bit integer compensation (returns P in Q24.8 Pa) */
static uint32_t compensate_P(int32_t adc_P)
{
    int64_t var1 = (int64_t)s_tfine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)s_cal.dig_P6;
    var2 += (var1 * (int64_t)s_cal.dig_P5) << 17;
    var2 += ((int64_t)s_cal.dig_P4) << 35;
    var1  = ((var1 * var1 * (int64_t)s_cal.dig_P3) >> 8)
           + ((var1 * (int64_t)s_cal.dig_P2) << 12);
    var1  = ((((int64_t)1 << 47) + var1) * (int64_t)s_cal.dig_P1) >> 33;
    if (var1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)s_cal.dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)s_cal.dig_P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)s_cal.dig_P7 << 4);
    return (uint32_t)p;  /* Units: Pa * 256 */
}

esp_err_t bmp280_init(void)
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
        .device_address  = BOARD_BMP280_I2C_ADDR,
        .scl_speed_hz    = BOARD_SENSOR_I2C_FREQ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Verify chip ID (BMP280=0x58, BME280=0x60, both supported) */
    uint8_t chip_id = 0;
    ret = bmp280_read_regs(BMP280_REG_ID, &chip_id, 1);
    if (ret != ESP_OK || (chip_id != 0x58 && chip_id != 0x60)) {
        ESP_LOGW(TAG, "BMP280 chip ID unexpected: 0x%02X (expected 0x58/0x60).", chip_id);
    }

    load_calib();

    /* Normal mode: osrs_t=001(1x), osrs_p=101(16x), mode=11(normal) */
    bmp280_write_reg(BMP280_REG_CTRL_MEAS, (1 << 5) | (5 << 2) | 3);
    /* IIR filter=010(4x), standby 62.5ms */
    bmp280_write_reg(BMP280_REG_CONFIG, (1 << 5) | (2 << 2));

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "BMP280 initialized (addr=0x%02X, chip=0x%02X).",
             BOARD_BMP280_I2C_ADDR, chip_id);
    return ESP_OK;
}

esp_err_t bmp280_read(float *pressure_hpa, float *temp_c)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    uint8_t raw[6] = {0};
    esp_err_t ret = bmp280_read_regs(BMP280_REG_DATA_START, raw, 6);
    if (ret != ESP_OK) return ret;

    int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);

    /* compensate_T must run first — it populates s_tfine used by compensate_P */
    int32_t  T_hundredths = compensate_T(adc_T);
    uint32_t P_q248       = compensate_P(adc_P);

    if (temp_c)       *temp_c       = (float)T_hundredths / 100.0f;
    if (pressure_hpa) *pressure_hpa = (float)P_q248       / 25600.0f; /* Pa*256 -> hPa */

    return ESP_OK;
}
