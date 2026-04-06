/**
 * @file cam_driver.h
 * @brief OV3660 camera hardware initialization.
 *
 * Thin wrapper around esp_camera_init() that reads pin config from the
 * board header and cam_config.h. Call cam_driver_init() once from app_main()
 * before starting the pipeline.
 */
#pragma once
#include "esp_err.h"
#include "esp_camera.h"

/**
 * @brief Initialize the OV3660 camera with board-specific pin mapping.
 *
 * On success, the esp_camera driver is running and frame buffers are
 * allocated in PSRAM. Call cam_driver_deinit() to shut down cleanly.
 *
 * @return ESP_OK on success
 *         ESP_ERR_NOT_FOUND if camera sensor is not detected (wrong pins?)
 *         other esp_err_t on driver failure
 */
esp_err_t cam_driver_init(void);

/**
 * @brief De-initialize the camera and release frame buffer memory.
 */
esp_err_t cam_driver_deinit(void);

/**
 * @brief Return true if the camera was initialized successfully.
 */
bool cam_driver_is_ready(void);

/**
 * @brief Get a pointer to the live camera sensor handle.
 * Returns NULL if camera is not initialized.
 * Use sensor->set_* functions to adjust brightness, contrast, etc.
 */
sensor_t *cam_driver_get_sensor(void);
