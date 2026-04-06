/**
 * @file stream_handler.h
 * @brief MJPEG multipart HTTP stream.
 *
 * Serves a continuous MJPEG stream on GET /stream.
 * Protected by Basic Auth.
 *
 * Each connected client gets the latest available JPEG frame from the
 * camera pipeline. The HTTP worker task runs on Core 0; frames are
 * acquired from the PSRAM frame buffer pool (Core 1 DMA side).
 */
#pragma once
#include "esp_http_server.h"

/**
 * @brief Register /stream (MJPEG) and /snapshot (single JPEG) handlers.
 * Called by http_server_start() — do not call directly.
 *
 * /stream  — continuous MJPEG multipart; works on desktop Chrome.
 *            iOS Safari stalls TCP receive window on MJPEG → do not use there.
 * /snapshot — single JPEG per request; JS polls this at the desired FPS.
 *             Immune to TCP stall: complete short response, iOS reads it
 *             immediately. Admin panel uses this for cross-platform 10 fps.
 */
void stream_handler_register(httpd_handle_t server);

/**
 * @brief Get current active MJPEG stream client count (approximate).
 */
uint32_t stream_handler_get_client_count(void);
