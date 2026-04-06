/**
 * @file aht20.h
 * @brief AHT20 temperature and humidity sensor driver (I2C).
 * Address: 0x38. Requires I2C bus to be initialized before use.
 *
 * Phase 1: STUB — returns placeholder readings.
 * Phase 3: Implement real I2C reads once sensor I2C pins are confirmed.
 */
#pragma once
#include "esp_err.h"

/** @brief Initialize AHT20. */
esp_err_t aht20_init(void);

/**
 * @brief Read temperature and humidity.
 * @param[out] temp_c     Temperature in Celsius.
 * @param[out] humidity   Relative humidity in percent.
 */
esp_err_t aht20_read(float *temp_c, float *humidity);
