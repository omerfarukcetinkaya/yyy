#pragma once
#include "esp_err.h"

esp_err_t status_reporter_init(void);
esp_err_t status_reporter_start(void);

/**
 * @brief Ensure HTTP server is running (idempotent).
 * Called by wifi_dual on every 2.4G IP acquisition so the server
 * binding is always fresh after band-switch netif flaps.
 */
void status_reporter_ensure_http_server(void);
