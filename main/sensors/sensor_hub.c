/**
 * @file sensor_hub.c
 * @brief Periodic sensor aggregation task — reads all sensors, classifies gas.
 */
#include "sensor_hub.h"
#include "i2c_bus.h"
#include "adc_bus.h"
#include "aht20.h"
#include "bmp280.h"
#include "mq7.h"
#include "mq137.h"
#include "veml7700.h"
#include "alarm/gas_analyzer.h"
#include "alarm/alarm_engine.h"
#include "telemetry_report.h"
#include "watchdog.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "sensor_hub";

static sensor_readings_t s_readings;
static SemaphoreHandle_t s_mutex;
static uint32_t s_read_count  = 0;
static uint32_t s_error_count = 0;
static bool     s_initialized = false;

/* Per-sensor fail counters for log throttling.
 * Log on first failure, then every SENSOR_ERR_LOG_INTERVAL polls (~60s at 2s poll). */
#define SENSOR_ERR_LOG_INTERVAL 30
static uint32_t s_aht20_fails   = 0;
static uint32_t s_bmp280_fails  = 0;
static uint32_t s_mq7_fails     = 0;
static uint32_t s_mq137_fails   = 0;
static uint32_t s_veml7700_fails = 0;

#define SENSOR_WARN(counter, fmt, ...) do {                             \
    (counter)++;                                                        \
    if ((counter) == 1 || (counter) % SENSOR_ERR_LOG_INTERVAL == 0)    \
        ESP_LOGW(TAG, fmt " (fail #%lu)", ##__VA_ARGS__,               \
                 (unsigned long)(counter));                              \
} while(0)

/* ── I2C reconnect state ─────────────────────────────────────────────────
 * After ~60 s of consecutive I2C failures, re-run init on all I2C sensors.
 * Retries indefinitely — sensor absence is a WARNING condition, never silent. */
#define SENSOR_REINIT_INTERVAL_POLLS  (60000U / CONFIG_VH_SENSOR_POLL_MS)
static uint32_t s_i2c_consec_fail = 0;

static void sensor_hub_read_all(void)
{
    float temp_c = 0, humidity = 0, pressure = 0;
    float co_ppm = 0, co_raw = 0;
    float nh3_ppm = 0, nh3_raw = 0;
    float lux = 0;
    bool any_error = false;
    bool i2c_ok    = false;   /* true when at least one I2C sensor succeeded */

    if (aht20_read(&temp_c, &humidity) == ESP_OK) {
        i2c_ok = true;
        s_aht20_fails = 0;
    } else {
        SENSOR_WARN(s_aht20_fails, "AHT20 read failed");
        any_error = true;
    }
    if (bmp280_read(&pressure, NULL) == ESP_OK) {
        i2c_ok = true;
        s_bmp280_fails = 0;
    } else {
        SENSOR_WARN(s_bmp280_fails, "BMP280 read failed");
        any_error = true;
    }
    if (mq7_read(&co_ppm, &co_raw) != ESP_OK) {
        SENSOR_WARN(s_mq7_fails, "MQ7 read failed");
        any_error = true;
    } else {
        s_mq7_fails = 0;
    }
    if (mq137_read(&nh3_ppm, &nh3_raw) != ESP_OK) {
        SENSOR_WARN(s_mq137_fails, "MQ137 read failed");
        any_error = true;
    } else {
        s_mq137_fails = 0;
    }
    if (veml7700_read(&lux) != ESP_OK) {
        SENSOR_WARN(s_veml7700_fails, "VEML7700 read failed");
        any_error = true;
    } else {
        s_veml7700_fails = 0;
    }

    /* ── I2C reconnect / alarm ──────────────────────────────────────────── */
    if (i2c_ok) {
        if (s_i2c_consec_fail > 0) {
            ESP_LOGI(TAG, "I2C sensors back online (after %lu consecutive polls offline).",
                     (unsigned long)s_i2c_consec_fail);
            /* Reset per-sensor counters so first failure is re-logged after recovery */
            s_aht20_fails = s_bmp280_fails = s_veml7700_fails = 0;
        }
        s_i2c_consec_fail = 0;
    } else {
        s_i2c_consec_fail++;
        /* Attempt reinit every ~60 s; never stop trying */
        if (s_i2c_consec_fail % SENSOR_REINIT_INTERVAL_POLLS == 0) {
            ESP_LOGW(TAG, "[ALARM] I2C sensors offline for ~%lu s. Attempting recovery ...",
                     (unsigned long)(s_i2c_consec_fail *
                                     CONFIG_VH_SENSOR_POLL_MS / 1000));
            /* Hardware bus recovery: 9 SCL pulses release any slave holding SDA
             * low after an incomplete transaction. i2c_bus_init() is idempotent
             * (no-op if already initialized) and cannot fix a stuck bus — this
             * must be done with i2c_bus_recover() first. */
            i2c_bus_recover();
            ESP_LOGI(TAG, "  AHT20:    %s",
                     aht20_init()    == ESP_OK ? "reinit OK" : "still offline");
            ESP_LOGI(TAG, "  BMP280:   %s",
                     bmp280_init()   == ESP_OK ? "reinit OK" : "still offline");
            ESP_LOGI(TAG, "  VEML7700: %s",
                     veml7700_init() == ESP_OK ? "reinit OK" : "still offline");
        }
    }

    /* Classify gas environment */
    gas_inputs_t gas_in = {
        .co_ppm  = co_ppm,
        .nh3_ppm = nh3_ppm,
        .temp_c  = temp_c,
        .lux     = lux,
    };
    gas_status_t gas = gas_analyzer_classify(&gas_in);
    const char *gas_str = gas_analyzer_status_str(gas);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readings.temp_c       = temp_c;
    s_readings.humidity_pct = humidity;
    s_readings.pressure_hpa = pressure;
    s_readings.co_ppm       = co_ppm;
    s_readings.co_raw       = co_raw;
    s_readings.nh3_ppm      = nh3_ppm;
    s_readings.nh3_raw      = nh3_raw;
    s_readings.lux          = lux;
    s_readings.gas_status   = gas;
    s_readings.valid        = i2c_ok;   /* only valid when I2C sensors respond */
    s_read_count++;
    if (any_error) s_error_count++;
    xSemaphoreGive(s_mutex);

    /* Feed alarm engine */
    alarm_engine_feed_sensors(&s_readings);

    /* Publish to telemetry */
    telemetry_set_sensors(temp_c, humidity, pressure, co_ppm, nh3_ppm, lux,
                          (uint8_t)gas, gas_str, s_read_count, s_error_count);
}

static void sensor_hub_task(void *arg)
{
    watchdog_subscribe_current_task();

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONFIG_VH_SENSOR_POLL_MS);

    ESP_LOGI(TAG, "Sensor task started (interval: %d ms).", CONFIG_VH_SENSOR_POLL_MS);

    while (true) {
        sensor_hub_read_all();
        watchdog_reset();
        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t sensor_hub_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    memset(&s_readings, 0, sizeof(s_readings));

    /* Initialize shared buses first */
    esp_err_t ret = i2c_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize individual drivers — log failures but continue */
    if (aht20_init()    != ESP_OK) ESP_LOGW(TAG, "AHT20 init failed.");
    if (bmp280_init()   != ESP_OK) ESP_LOGW(TAG, "BMP280 init failed.");
    if (mq7_init()      != ESP_OK) ESP_LOGW(TAG, "MQ7 init failed.");
    if (mq137_init()    != ESP_OK) ESP_LOGW(TAG, "MQ137 init failed.");
    if (veml7700_init() != ESP_OK) ESP_LOGW(TAG, "VEML7700 init failed.");

    s_initialized = true;
    ESP_LOGI(TAG, "Sensor hub initialized (AHT20+BMP280+MQ7+MQ137+VEML7700).");
    return ESP_OK;
}

esp_err_t sensor_hub_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        sensor_hub_task, "sensor_hub",
        6144, NULL,
        3, NULL,
        0   /* Core 0 */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void sensor_hub_get_readings(sensor_readings_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_readings, sizeof(sensor_readings_t));
    xSemaphoreGive(s_mutex);
}

void sensor_hub_get_stats(uint32_t *reads, uint32_t *errors)
{
    if (reads)  *reads  = s_read_count;
    if (errors) *errors = s_error_count;
}
