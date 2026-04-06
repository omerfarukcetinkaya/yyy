/**
 * @file veml7700.h
 * @brief VEML7700 ambient light sensor driver (I2C, address 0x10).
 *
 * Measures illuminance in lux. A sudden lux spike (fire, flashlight) combined
 * with elevated temperature or gas readings can indicate a fire scenario.
 */
#pragma once
#include "esp_err.h"

/** @brief Initialize VEML7700 on the shared I2C bus. */
esp_err_t veml7700_init(void);

/**
 * @brief Read ambient illuminance.
 * @param[out] lux   Illuminance in lux (0.0 to ~120,000 lux).
 */
esp_err_t veml7700_read(float *lux);
