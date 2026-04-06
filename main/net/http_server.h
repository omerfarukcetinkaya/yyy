/**
 * @file http_server.h
 * @brief Central HTTP server lifecycle and route registration.
 *
 * Manages a single esp_httpd instance. Routes are registered by individual
 * handler modules (stream_handler, admin_panel, web_reporter) which call
 * http_server_register_uri() after the server is started.
 *
 * The server is started by wifi_manager when an IP is obtained, and stopped
 * on Wi-Fi loss. All registered URIs survive stop/start cycles.
 */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Start the HTTP server and register all known URI handlers.
 * Safe to call multiple times (idempotent if already running).
 * @return ESP_OK on success.
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop the HTTP server. Existing connections are closed.
 */
void http_server_stop(void);

/**
 * @brief Return true if the server is currently running.
 */
bool http_server_is_running(void);

/**
 * @brief Get the raw server handle (for advanced use only).
 * Returns NULL if server is not running.
 */
httpd_handle_t http_server_get_handle(void);

/**
 * @brief Register a URI handler. Can be called before or after start.
 * Handlers registered before start are applied when the server starts.
 * Returns ESP_OK if registered (may be deferred if server not yet running).
 */
esp_err_t http_server_register_uri(const httpd_uri_t *uri_handler);
