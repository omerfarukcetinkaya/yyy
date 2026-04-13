/**
 * @file mq7.h
 * @brief MQ7 carbon monoxide sensor driver (ADC).
 *
 * MQ7 requires a heating cycle (5V / 1.4V alternating) for accurate readings.
 * Phase 1: STUB — returns ADC raw value with a rough ppm estimate.
 * Phase 3: Implement heating cycle control and calibrated ppm conversion.
 */
#pragma once
#include "esp_err.h"

/** @brief Initialize MQ7 ADC channel. */
esp_err_t mq7_init(void);

/**
 * @brief Read MQ7 output.
 * @param[out] co_ppm   Estimated CO concentration in ppm (rough, uncalibrated).
 * @param[out] raw_mv   Raw ADC reading in millivolts.
 */
esp_err_t mq7_read(float *co_ppm, float *raw_mv);
