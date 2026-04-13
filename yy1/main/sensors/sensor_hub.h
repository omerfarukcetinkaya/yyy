/**
 * @file sensor_hub.h
 * @brief Periodic sensor aggregation task.
 *
 * Owns the I2C bus, ADC bus, and all sensor drivers.
 * Reads all sensors on a configurable interval, runs gas_analyzer,
 * and publishes results to the telemetry module and alarm engine.
 *
 * Sensors managed:
 *   AHT20  — temperature + humidity (I2C)
 *   BMP280 — pressure (I2C)
 *   MQ7    — CO gas (ADC1_CH0)
 *   MQ137  — NH3/VOC gas (ADC1_CH1)
 *   VEML7700 — ambient light / lux (I2C)
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "alarm/gas_analyzer.h"

typedef struct {
    float temp_c;
    float humidity_pct;
    float pressure_hpa;
    float co_ppm;
    float co_raw;
    float nh3_ppm;
    float nh3_raw;
    float lux;
    gas_status_t gas_status;
    /* True only when at least one environmental sensor (AHT20 or BMP280)
     * succeeded this cycle. False = all I2C sensors errored (hardware absent
     * or cold boot). Alarm engine must not fire on invalid readings. */
    bool valid;
} sensor_readings_t;

/**
 * @brief Initialize the sensor hub (I2C bus, ADC bus, all driver inits).
 * Returns ESP_OK even if individual sensors fail — errors are counted.
 */
esp_err_t sensor_hub_init(void);

/**
 * @brief Start the sensor polling task (Core 0, low priority).
 */
esp_err_t sensor_hub_start(void);

/**
 * @brief Get the latest sensor readings (snapshot, thread-safe).
 */
void sensor_hub_get_readings(sensor_readings_t *out);

/**
 * @brief Get total read count and error count since boot.
 */
void sensor_hub_get_stats(uint32_t *reads, uint32_t *errors);
