/**
 * @file mq137.h
 * @brief MQ137 ammonia / VOC gas sensor driver (ADC1).
 *
 * MQ137 is sensitive to NH3 (ammonia) and various organic vapors.
 * Useful for detecting burning plastic/cable (VOC spike) and toxic gas events.
 * Same resistive measurement approach as MQ7.
 */
#pragma once
#include "esp_err.h"

/** @brief Initialize MQ137 ADC channel. */
esp_err_t mq137_init(void);

/**
 * @brief Read MQ137 output.
 * @param[out] nh3_ppm   Estimated NH3/VOC concentration in ppm.
 * @param[out] raw_mv    Raw ADC reading in millivolts.
 */
esp_err_t mq137_read(float *nh3_ppm, float *raw_mv);
