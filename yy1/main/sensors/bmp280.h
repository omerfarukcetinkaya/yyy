/**
 * @file bmp280.h
 * @brief BMP280 barometric pressure sensor driver (I2C).
 * Address: 0x76 or 0x77 depending on SDO pin.
 *
 * Phase 1: STUB — returns placeholder readings.
 * Phase 3: Implement real I2C reads once sensor I2C pins are confirmed.
 */
#pragma once
#include "esp_err.h"

/** @brief Initialize BMP280. */
esp_err_t bmp280_init(void);

/**
 * @brief Read pressure and temperature.
 * @param[out] pressure_hpa  Barometric pressure in hPa.
 * @param[out] temp_c        Temperature in Celsius (cross-check with AHT20).
 */
esp_err_t bmp280_read(float *pressure_hpa, float *temp_c);
