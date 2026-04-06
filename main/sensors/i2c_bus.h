/**
 * @file i2c_bus.h
 * @brief Shared I2C master bus for all sensor/peripheral I2C devices.
 *
 * Creates I2C_NUM_0 with the board-defined SDA/SCL pins (GPIO21/GPIO47).
 * All I2C drivers (AHT20, BMP280, VEML7700, SSD1306) obtain the shared
 * bus handle and add their own device handle via i2c_master_bus_add_device().
 *
 * Call i2c_bus_init() once from sensor_hub_init() before any driver inits.
 */
#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * @brief Initialize the shared I2C master bus.
 * Idempotent — safe to call multiple times.
 */
esp_err_t i2c_bus_init(void);

/**
 * @brief Return the shared bus handle.
 * Must be called after i2c_bus_init(). Returns NULL if not initialized.
 */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/**
 * @brief Hardware bus recovery — generate 9 SCL pulses to release any slave
 * holding SDA low (stuck after an incomplete transaction).
 * Call before re-initialising sensors when reads have been failing.
 */
esp_err_t i2c_bus_recover(void);
