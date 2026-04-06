/**
 * @file cam_pipeline.h
 * @brief Camera frame buffer pipeline: acquisition statistics and consumer API.
 *
 * The esp_camera driver manages frame buffer acquisition via DMA. This module
 * wraps esp_camera_fb_get/return with FPS tracking, drop counting, and a
 * clean typed interface for pipeline consumers (streamer, motion detector, etc).
 *
 * Ownership model:
 *   - cam_pipeline_get_frame() returns a frame with a reference held.
 *   - The caller MUST call cam_pipeline_release_frame() when done.
 *   - A frame should not be held across task suspensions longer than ~200 ms
 *     to avoid starving the frame buffer pool.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"

/**
 * @brief A wrapped camera frame.
 * `fb` is owned by the esp_camera driver and must be returned via
 * cam_pipeline_release_frame().
 */
typedef struct {
    camera_fb_t *fb;        /**< Raw driver frame buffer */
    uint32_t     seq;       /**< Monotonic acquisition sequence number */
    int64_t      ts_us;     /**< Capture timestamp (esp_timer_get_time) */
} cam_frame_t;

/**
 * @brief Pipeline statistics snapshot.
 */
typedef struct {
    uint32_t frame_count;   /**< Total frames acquired since boot */
    uint32_t drop_count;    /**< Frames where fb_get timed out */
    uint32_t fps_1s;        /**< Frames acquired in the last second */
} cam_pipeline_stats_t;

/**
 * @brief Initialize pipeline state (stats counters, mutex).
 * Must be called before cam_driver_init().
 */
esp_err_t cam_pipeline_init(void);

/**
 * @brief Acquire the next frame from the camera.
 *
 * Wraps esp_camera_fb_get() with timeout and statistics tracking.
 * Blocks for at most timeout_ticks waiting for a frame.
 *
 * @param[out] frame        Populated on success.
 * @param[in]  timeout_ms   Maximum wait in milliseconds.
 * @return ESP_OK           Frame acquired.
 *         ESP_ERR_TIMEOUT  No frame available within timeout.
 *         ESP_FAIL         Camera not initialized.
 */
esp_err_t cam_pipeline_get_frame(cam_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief Return a frame to the driver, making the buffer available for reuse.
 * Must be called exactly once per successful cam_pipeline_get_frame().
 */
void cam_pipeline_release_frame(cam_frame_t *frame);

/**
 * @brief Get a snapshot of pipeline statistics (thread-safe).
 */
void cam_pipeline_get_stats(cam_pipeline_stats_t *out);
