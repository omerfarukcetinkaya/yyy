/**
 * @file web_reporter.h
 * @brief JSON telemetry endpoint at GET /api/telemetry.
 * Returns the latest telemetry_t snapshot as a structured JSON document.
 * Protected by Basic Auth.
 */
#pragma once
#include "esp_http_server.h"

/** @brief Register /api/telemetry URI handler. */
void web_reporter_register(httpd_handle_t server);
