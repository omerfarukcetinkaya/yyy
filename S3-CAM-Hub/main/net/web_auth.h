/**
 * @file web_auth.h
 * @brief HTTP Basic Auth for the admin panel.
 *
 * DEV CREDENTIALS — isolated here deliberately.
 * Replace CONFIG_VH_WEB_USERNAME / CONFIG_VH_WEB_PASSWORD via menuconfig
 * before any production deployment.
 *
 * The pre-computed Base64 token is generated at init time from the
 * Kconfig credentials so no runtime encoding is needed per request.
 */
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

/**
 * @brief Initialize auth module: precompute the expected Authorization header.
 * Call once before starting the HTTP server.
 */
void web_auth_init(void);

/**
 * @brief Check whether an incoming HTTP request carries valid Basic Auth.
 * @return true if credentials match, false otherwise.
 */
bool web_auth_check(httpd_req_t *req);

/**
 * @brief Send HTTP 401 Unauthorized with WWW-Authenticate challenge.
 * Call this when web_auth_check() returns false.
 */
void web_auth_send_challenge(httpd_req_t *req);

/**
 * @brief Set the session cookie on a response so the browser remembers
 * the session without re-prompting for credentials.
 * Call after a successful auth check before sending the response body.
 */
void web_auth_set_session_cookie(httpd_req_t *req);

/**
 * @brief Clear the session cookie (logout).
 */
void web_auth_clear_session_cookie(httpd_req_t *req);
