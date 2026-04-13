/**
 * @file web_reporter.c
 * @brief JSON telemetry endpoint at GET /api/telemetry.
 */
#include "web_reporter.h"
#include "web_auth.h"
#include "telemetry_report.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "web_rpt";

/* JSON buffer — allocated on stack in handler; sized for worst case. */
#define JSON_BUF_SIZE  2200

static esp_err_t telemetry_get_handler(httpd_req_t *req)
{
    ESP_LOGD(TAG, "/api/telemetry request received.");
    if (!web_auth_check(req)) {
        ESP_LOGW(TAG, "/api/telemetry: auth failed.");
        web_auth_send_challenge(req);
        return ESP_OK;
    }

    telemetry_t t;
    telemetry_snapshot(&t);

    char buf[JSON_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf),
        "{"
          "\"system\":{"
            "\"uptime_s\":%lu,"
            "\"reset_reason\":\"%s\""
          "},"
          "\"wifi\":{"
            "\"connected\":%s,"
            "\"ip\":\"%s\","
            "\"rssi_dbm\":%d,"
            "\"reconnect_count\":%lu"
          "},"
          "\"camera\":{"
            "\"fps_1s\":%lu,"
            "\"frame_count\":%lu,"
            "\"drop_count\":%lu"
          "},"
          "\"vision\":{"
            "\"motion_fps_1s\":%lu,"
            "\"motion_detected\":%s,"
            "\"motion_score\":%.3f,"
            "\"classifier_fps_1s\":%lu,"
            "\"classifier_label\":\"%s\","
            "\"classifier_confidence\":%.3f"
          "},"
          "\"stream\":{"
            "\"client_count\":%lu"
          "},"
          "\"sensors\":{"
            "\"temp_c\":%.2f,"
            "\"humidity_pct\":%.2f,"
            "\"pressure_hpa\":%.2f,"
            "\"co_ppm\":%.2f,"
            "\"nh3_ppm\":%.2f,"
            "\"lux\":%.1f,"
            "\"gas_status\":%u,"
            "\"gas_status_str\":\"%s\","
            "\"read_count\":%lu,"
            "\"error_count\":%lu"
          "},"
          "\"cpu\":{"
            "\"core0_pct\":%u,"
            "\"core1_pct\":%u"
          "},"
          "\"memory\":{"
            "\"heap_free_b\":%lu,"
            "\"heap_free_min_b\":%lu,"
            "\"heap_largest_block_b\":%lu,"
            "\"internal_free_b\":%lu,"
            "\"psram_free_b\":%lu,"
            "\"psram_free_min_b\":%lu,"
            "\"psram_largest_block_b\":%lu"
          "},"
          "\"alarm\":{"
            "\"active\":%s,"
            "\"count\":%lu,"
            "\"last_reason\":\"%s\","
            "\"telegram_sent\":%lu,"
            "\"telegram_fail\":%lu"
          "},"
          "\"errors\":{"
            "\"cam_init\":%lu,"
            "\"cam_fb_timeout\":%lu,"
            "\"wifi_fail\":%lu,"
            "\"sensor_read\":%lu"
          "}"
        "}",
        (unsigned long)t.uptime_s, t.reset_reason,
        t.wifi_connected ? "true" : "false",
        t.wifi_ip, t.wifi_rssi_dbm,
        (unsigned long)t.wifi_reconnect_count,
        (unsigned long)t.cam_fps_1s,
        (unsigned long)t.cam_frame_count,
        (unsigned long)t.cam_drop_count,
        (unsigned long)t.motion_fps_1s,
        t.motion_detected ? "true" : "false",
        t.motion_score,
        (unsigned long)t.classifier_fps_1s,
        t.classifier_label,
        t.classifier_confidence,
        (unsigned long)t.stream_client_count,
        t.temp_c, t.humidity_pct, t.pressure_hpa, t.co_ppm,
        t.nh3_ppm, t.lux, (unsigned)t.gas_status, t.gas_status_str,
        (unsigned long)t.sensor_read_count,
        (unsigned long)t.sensor_error_count,
        (unsigned)t.cpu_core0_pct,
        (unsigned)t.cpu_core1_pct,
        (unsigned long)t.heap_free_b,
        (unsigned long)t.heap_free_min_b,
        (unsigned long)t.heap_largest_block_b,
        (unsigned long)t.internal_free_b,
        (unsigned long)t.psram_free_b,
        (unsigned long)t.psram_free_min_b,
        (unsigned long)t.psram_largest_block_b,
        t.alarm_active ? "true" : "false",
        (unsigned long)t.alarm_count,
        t.alarm_last_reason,
        (unsigned long)t.telegram_sent_count,
        (unsigned long)t.telegram_fail_count,
        (unsigned long)t.err_cam_init,
        (unsigned long)t.err_cam_fb_timeout,
        (unsigned long)t.err_wifi_fail,
        (unsigned long)t.err_sensor_read
    );

    if (n >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "JSON truncated (%d >= %d).", n, JSON_BUF_SIZE);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static const httpd_uri_t s_telem_uri = {
    .uri     = "/api/telemetry",
    .method  = HTTP_GET,
    .handler = telemetry_get_handler,
};

void web_reporter_register(httpd_handle_t server)
{
    httpd_register_uri_handler(server, &s_telem_uri);
    ESP_LOGI(TAG, "/api/telemetry registered.");
}
