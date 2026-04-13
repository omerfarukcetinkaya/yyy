/**
 * @file admin_panel.h
 * @brief Authenticated HTML admin dashboard (GET /).
 *
 * Serves a static HTML page with:
 *   - Live MJPEG stream (src="/stream")
 *   - Telemetry dashboard (polls /api/telemetry every 1s via JS fetch)
 *   - Protected by Basic Auth
 */
#pragma once
#include "esp_http_server.h"

/**
 * @brief Register / and /index.html URI handlers.
 * Called by http_server_start() — do not call directly.
 */
void admin_panel_register(httpd_handle_t server);
