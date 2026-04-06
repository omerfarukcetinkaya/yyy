/**
 * @file motion_detect.h
 * @brief Motion detection pipeline (frame differencing).
 *
 * Phase 1: STUB — always reports no motion.
 * Phase 4: Implement frame differencing on downscaled JPEG-decoded frames.
 *
 * Design:
 *   - Runs on Core 1 at ~5 FPS (MOTION_SAMPLE_PERIOD_MS)
 *   - Acquires a frame from cam_pipeline
 *   - Compares to previous frame (luminance difference)
 *   - Reports motion score [0.0, 1.0] and bounding region
 *   - Feeds alarm_engine_feed_motion() on detection
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float    score;          /**< Motion intensity [0.0, 1.0] */
    bool     detected;       /**< True if score > threshold */
    uint16_t roi_x, roi_y;  /**< Top-left of motion region */
    uint16_t roi_w, roi_h;  /**< Width/height of motion region */
    uint32_t fps_1s;         /**< Motion pipeline FPS (last 1s) */
} motion_result_t;

/** @brief Initialize motion detection state. */
esp_err_t motion_detect_init(void);

/** @brief Start the motion detection task on Core 1. */
esp_err_t motion_detect_start(void);

/** @brief Get latest motion result (thread-safe snapshot). */
void motion_detect_get_result(motion_result_t *out);
