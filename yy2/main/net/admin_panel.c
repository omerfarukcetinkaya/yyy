/**
 * @file admin_panel.c
 * @brief Scout admin dashboard — served on 2.4G network.
 *
 * Basic Auth: tenten / 1234 (same as S3 for convenience).
 * Auto-refreshing dashboard showing bridge status, S3 status,
 * alarm state, WiFi band, and Telegram mute state.
 */
#include "admin_panel.h"
#include "wifi_dual.h"
#include "telegram_client.h"
#include "espnow_bridge.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "scout_admin";

/* Pre-computed Basic Auth: tenten:1234 → base64 */
static char s_expected_auth[128] = {0};
static bool s_auth_init = false;

static void init_auth(void)
{
    if (s_auth_init) return;
    const char *creds = CONFIG_SCOUT_S3_USER ":" CONFIG_SCOUT_S3_PASS;
    unsigned char b64[96] = {0};
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                          (const unsigned char *)creds, strlen(creds));
    snprintf(s_expected_auth, sizeof(s_expected_auth), "Basic %.*s",
             (int)b64_len, b64);
    s_auth_init = true;
}

static bool check_auth(httpd_req_t *req)
{
    init_auth();
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }
    return (strncmp(auth_hdr, s_expected_auth, strlen(s_expected_auth)) == 0);
}

static void send_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Scout Bridge\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized.");
}

static const char SCOUT_HTML[] =
"<!DOCTYPE html>"
"<html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<meta http-equiv=refresh content=5>"
"<title>Scout Bridge</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#0a0a1a;color:#4fc3f7;font-family:'Courier New',monospace;padding:16px}"
"h1{color:#81d4fa;margin-bottom:16px;font-size:1.2em;letter-spacing:2px}"
".card{background:#111;border:1px solid #1565c0;border-radius:6px;padding:14px;margin-bottom:12px}"
".card h2{color:#64b5f6;font-size:0.85em;margin-bottom:8px;border-bottom:1px solid #1565c0;padding-bottom:4px}"
".row{display:flex;justify-content:space-between;padding:2px 0;font-size:0.78em}"
".lbl{color:#4db6ac}.val{color:#e0e0e0}"
".ok{color:#00e676}.warn{color:#ffeb3b}.alarm{color:#f44336}"
".badge{display:inline-block;padding:2px 8px;border-radius:3px;font-size:0.7em;font-weight:bold}"
".bg-ok{background:#1b5e20;color:#69f0ae}"
".bg-warn{background:#e65100;color:#ffe0b2}"
".bg-alarm{background:#b71c1c;color:#ffcdd2}"
"</style></head><body>"
"<h1>&#128737; Scout Bridge — ESP32-C5</h1>";

static esp_err_t admin_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_challenge(req);
        return ESP_OK;
    }

    /* Heap-alloc 2KB buffer — httpd task stack is 8KB, keep it out of stack */
    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int n = 0;
    n += snprintf(buf + n, 2048 - n, "%s", SCOUT_HTML);

    /* WiFi card */
    bool conn = wifi_dual_is_connected();
    bool on5g = wifi_dual_is_on_5g();
    n += snprintf(buf + n, 2048 - n,
        "<div class=card><h2>WIFI</h2>"
        "<div class=row><span class=lbl>Status</span><span class='%s'>%s</span></div>"
        "<div class=row><span class=lbl>Band</span><span class=val>%s</span></div>"
        "<div class=row><span class=lbl>IP</span><span class=val>%s</span></div>"
        "<div class=row><span class=lbl>RSSI</span><span class=val>%d dBm</span></div>"
        "</div>",
        conn ? "ok" : "alarm", conn ? "Connected" : "DISCONNECTED",
        on5g ? "5 GHz (Internet)" : "2.4 GHz (IoT)",
        wifi_dual_get_ip(), (int)wifi_dual_get_rssi());

    /* S3 card */
    bool s3_on = espnow_bridge_is_s3_online();
    bool alarm = espnow_bridge_alarm_active();
    n += snprintf(buf + n, 2048 - n,
        "<div class=card><h2>S3 VISION HUB</h2>"
        "<div class=row><span class=lbl>Status</span><span class='%s'>%s</span></div>"
        "<div class=row><span class=lbl>Alarm</span><span class='%s'>%s</span></div>"
        "</div>",
        s3_on ? "ok" : "alarm", s3_on ? "ONLINE" : "OFFLINE",
        alarm ? "alarm" : "ok",
        alarm ? espnow_bridge_alarm_reason() : "Clear");

    /* Telegram card */
    n += snprintf(buf + n, 2048 - n,
        "<div class=card><h2>TELEGRAM</h2>"
        "<div class=row><span class=lbl>Muted</span><span class='%s'>%s</span></div>"
        "<div class=row><span class=lbl>Schedule</span><span class=val>%02d:00 - %02d:00</span></div>"
        "</div>",
        telegram_is_muted() ? "warn" : "ok",
        telegram_is_muted() ? "MUTED" : "Active",
        CONFIG_SCOUT_SCHEDULE_START_HOUR, CONFIG_SCOUT_SCHEDULE_END_HOUR);

    /* System card */
    n += snprintf(buf + n, 2048 - n,
        "<div class=card><h2>SYSTEM</h2>"
        "<div class=row><span class=lbl>Uptime</span><span class=val>%lu s</span></div>"
        "<div class=row><span class=lbl>Heap Free</span><span class=val>%lu B</span></div>"
        "</div>",
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size());

    n += snprintf(buf + n, 2048 - n,
        "<p style='color:#546e7a;font-size:0.6em;margin-top:12px'>Auto-refresh 5s</p>"
        "</body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static const httpd_uri_t s_root = {
    .uri = "/", .method = HTTP_GET, .handler = admin_handler,
};

static const httpd_uri_t s_index = {
    .uri = "/index.html", .method = HTTP_GET, .handler = admin_handler,
};

void scout_admin_register(httpd_handle_t server)
{
    httpd_register_uri_handler(server, &s_root);
    httpd_register_uri_handler(server, &s_index);
    ESP_LOGI(TAG, "Scout admin panel registered at '/'");
}
