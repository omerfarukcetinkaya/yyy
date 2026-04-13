/**
 * @file ws_stream.h
 * @brief WebSocket binary video broadcaster at /ws/stream.
 *
 * Each connected client spawns a dedicated sender task. Senders pull
 * frames from frame_pool (zero-copy refcount) and push them as WS
 * binary frames via httpd_ws_send_frame_async. Text frames carry
 * motion detection JSON so the browser can render bounding-box overlays.
 *
 * Supports up to WS_STREAM_MAX_CLIENTS concurrent viewers (default 2).
 * Senders run on Core 0 (same as httpd worker) at priority 5.
 */
#pragma once
#include "esp_http_server.h"
#include <stdint.h>

/** @brief Register /ws/stream on the given httpd handle. */
void ws_stream_register(httpd_handle_t server);

/** @brief Current connected WebSocket client count. */
uint32_t ws_stream_get_client_count(void);
