/**
 * @file stream_handler.h
 * @brief Simple single-shot JPEG snapshot endpoint.
 *
 * GET /snapshot returns the latest camera frame from frame_pool.
 * Protected by Basic Auth. Used as a fallback for scripts / debug.
 *
 * The primary live view is /ws/stream (WebSocket binary push).
 */
#pragma once
#include "esp_http_server.h"
#include <stdint.h>

/** @brief Register /snapshot. Called by http_server_start(). */
void stream_handler_register(httpd_handle_t server);
