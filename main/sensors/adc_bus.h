/**
 * @file adc_bus.h
 * @brief Shared ADC1 oneshot unit for MQ7 and MQ137 sensors.
 *
 * ADC1 unit handle is created once. Both MQ sensors configure their
 * channels independently, then call adc_bus_read() to sample.
 *
 * Using ESP-IDF v5 oneshot API (esp_adc/adc_oneshot.h).
 * ADC2 cannot be used while Wi-Fi is active — ADC1 only.
 */
#pragma once
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

/**
 * @brief Initialize ADC1 unit. Idempotent.
 */
esp_err_t adc_bus_init(void);

/**
 * @brief Configure a single ADC1 channel (call once per sensor during init).
 */
esp_err_t adc_bus_config_channel(adc_channel_t ch);

/**
 * @brief Read a raw sample from an ADC1 channel.
 * @param[out] raw  12-bit ADC value (0-4095).
 */
esp_err_t adc_bus_read(adc_channel_t ch, int *raw);
