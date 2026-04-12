/**
 * @file cam_pipeline.h
 * @brief Camera DMA producer task: drains frames into frame_pool.
 *
 * The pipeline runs a dedicated producer task on Core 1 that continuously
 * calls esp_camera_fb_get() and copies each JPEG into a frame_pool slot.
 * Consumers (WebSocket stream, motion detector, /snapshot handler) never
 * touch the camera driver directly — they use frame_pool_get_latest() or
 * frame_pool_wait_newer() and only see stable, refcounted JPEG slots.
 *
 * Ownership model:
 *   - cam_pipeline_init()  prepares state (call before frame_pool_init)
 *   - cam_pipeline_start() creates the producer task (call after
 *     frame_pool_init and cam_driver_init)
 *   - Statistics (fps, frame_count, drop_count) are tracked for telemetry.
 *   - GDMA freeze recovery: after N consecutive esp_camera_fb_get() NULLs
 *     the producer reinitializes the camera driver.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Pipeline statistics snapshot (used by telemetry).
 */
typedef struct {
    uint32_t frame_count;   /**< Total frames acquired since boot */
    uint32_t drop_count;    /**< fb_get timeouts + frame_pool acquire fails */
    uint32_t fps_1s;        /**< Frames acquired in the last second */
} cam_pipeline_stats_t;

/**
 * @brief Initialize pipeline state (stats mutex). Idempotent.
 * Must be called once from app_main() before frame_pool_init().
 */
esp_err_t cam_pipeline_init(void);

/**
 * @brief Start the camera producer task on Core 1 at high priority.
 * Must be called AFTER frame_pool_init() and cam_driver_init().
 */
esp_err_t cam_pipeline_start(void);

/**
 * @brief Thread-safe stats snapshot.
 */
void cam_pipeline_get_stats(cam_pipeline_stats_t *out);
