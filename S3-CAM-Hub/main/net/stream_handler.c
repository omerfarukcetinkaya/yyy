/**
 * @file stream_handler.c
 * @brief /snapshot — single JPEG per request, sourced from frame_pool.
 *
 * The primary live-view path is /ws/stream (see ws_stream.c). /snapshot is
 * retained as a lightweight fallback endpoint for curl scripts, browser
 * address-bar testing and simple clients.
 */
#include "stream_handler.h"
#include "web_auth.h"
#include "frame_pool.h"
#include "cam_driver.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "snapshot";

static esp_err_t snapshot_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        web_auth_send_challenge(req);
        return ESP_OK;
    }

    if (!cam_driver_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Camera not ready.");
        return ESP_OK;
    }

    frame_slot_t *slot = frame_pool_get_latest();
    if (!slot) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "No frame available.");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t ret = httpd_resp_send(req, (const char *)slot->buf, (ssize_t)slot->len);
    frame_pool_release(slot);
    return ret;
}

static const httpd_uri_t s_snapshot_uri = {
    .uri     = "/snapshot",
    .method  = HTTP_GET,
    .handler = snapshot_get_handler,
    .user_ctx = NULL,
};

void stream_handler_register(httpd_handle_t server)
{
    esp_err_t ret = httpd_register_uri_handler(server, &s_snapshot_uri);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register /snapshot: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "/snapshot registered (frame_pool fallback).");
    }
}
