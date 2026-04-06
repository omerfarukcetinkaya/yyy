/**
 * @file classifier.h
 * @brief Object classification interface (~1 FPS).
 *
 * Phase 1: STUB — returns "none" with 0.0 confidence.
 * Phase 5+: Integrate TFLite micro model or ESP-DL model.
 *
 * Planned classes: human, cat, [others].
 * The interface is designed to be model-agnostic.
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

#define CLASSIFIER_MAX_CLASSES  8
#define CLASSIFIER_LABEL_LEN    32

typedef struct {
    char  label[CLASSIFIER_LABEL_LEN];
    float confidence;   /**< [0.0, 1.0] */
} classifier_prediction_t;

typedef struct {
    classifier_prediction_t top1;
    uint32_t                inference_ms;  /**< Last inference time in ms */
    uint32_t                fps_1s;
} classifier_result_t;

esp_err_t classifier_init(void);
esp_err_t classifier_start(void);
void classifier_get_result(classifier_result_t *out);
