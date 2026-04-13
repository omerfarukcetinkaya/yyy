/**
 * @file espnow_bridge.c
 * @brief ESP-NOW receiver — listens for alarm/status packets from S3 (yy1)
 *        and other future ESP32 devices on the local 2.4G mesh.
 *
 * Also polls S3's HTTP /api/telemetry endpoint when on 2.4G band.
 *
 * Protocol: each ESP-NOW packet is a fixed struct:
 *   { uint8_t type; uint8_t device_id; uint16_t payload_len; uint8_t payload[240]; }
 *   type: 0x01=alarm, 0x02=status, 0x03=telemetry
 */
#include "espnow_bridge.h"
#include "telegram_client.h"
#include "wifi_dual.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "espnow_brg";

#define S3_TELEMETRY_URL  "http://" CONFIG_SCOUT_S3_IP "/api/telemetry"
#define S3_POLL_INTERVAL_MS  10000   /* poll S3 every 10s when on 2.4G */
#define TELEMETRY_HISTORY   5

static struct {
    char    telemetry_history[TELEMETRY_HISTORY][512];
    int     history_idx;
    int     history_count;
    bool    s3_online;
    int64_t last_s3_poll_us;
    /* Last alarm info */
    bool    alarm_active;
    char    alarm_reason[128];
} s_bridge;

/* ── HTTP response accumulator ─────────────────────────────────────────── */
static char s_http_buf[2048];
static int  s_http_len;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_http_len + evt->data_len < (int)sizeof(s_http_buf)) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* ── Poll S3 telemetry over HTTP (2.4G) ───────────────────────────────── */

static void poll_s3_telemetry(void)
{
    if (!wifi_dual_is_connected() || wifi_dual_is_on_5g()) return;

    s_http_len = 0;
    esp_http_client_config_t cfg = {
        .url = S3_TELEMETRY_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_evt,
        .timeout_ms = 5000,
        .buffer_size = 1024,
        .username = CONFIG_SCOUT_S3_USER,
        .password = CONFIG_SCOUT_S3_PASS,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200 && s_http_len > 0) {
        s_http_buf[s_http_len] = '\0';
        s_bridge.s3_online = true;

        /* Store in history ring */
        int idx = s_bridge.history_idx % TELEMETRY_HISTORY;
        strncpy(s_bridge.telemetry_history[idx], s_http_buf,
                sizeof(s_bridge.telemetry_history[idx]) - 1);
        s_bridge.history_idx++;
        if (s_bridge.history_count < TELEMETRY_HISTORY)
            s_bridge.history_count++;

        /* Check for alarm in telemetry JSON */
        bool alarm_active = (strstr(s_http_buf, "\"active\":true") != NULL);
        if (alarm_active && !s_bridge.alarm_active) {
            /* New alarm detected! Relay to Telegram */
            const char *reason = strstr(s_http_buf, "\"last_reason\":\"");
            char reason_buf[128] = "Motion detected";
            if (reason) {
                reason += 15;
                int i = 0;
                while (*reason && *reason != '"' && i < 127) {
                    reason_buf[i++] = *reason++;
                }
                reason_buf[i] = '\0';
            }
            strncpy(s_bridge.alarm_reason, reason_buf, sizeof(s_bridge.alarm_reason) - 1);

            char alert_msg[256];
            snprintf(alert_msg, sizeof(alert_msg),
                     "S3 Vision Hub: %s", reason_buf);
            telegram_send_alert(alert_msg);
        }
        s_bridge.alarm_active = alarm_active;

        ESP_LOGD(TAG, "S3 telemetry polled OK (%d bytes)", s_http_len);
    } else {
        s_bridge.s3_online = false;
        ESP_LOGW(TAG, "S3 poll failed: err=%s status=%d", esp_err_to_name(err), status);
    }
}

/* ── Status/telemetry callbacks for Telegram ──────────────────────────── */

static void status_callback(char *buf, size_t buflen)
{
    snprintf(buf, buflen,
        "📊 <b>SecBridge Status</b>\\n\\n"
        "WiFi: %s (%s)\\n"
        "RSSI: %d dBm\\n"
        "S3 Vision Hub: %s\\n"
        "Alarm: %s\\n"
        "Muted: %s",
        wifi_dual_is_connected() ? "Connected" : "Disconnected",
        wifi_dual_is_on_5g() ? "5G Internet" : "2.4G IoT",
        (int)wifi_dual_get_rssi(),
        s_bridge.s3_online ? "ONLINE" : "OFFLINE",
        s_bridge.alarm_active ? s_bridge.alarm_reason : "Clear",
        telegram_is_muted() ? "Yes" : "No");
}

static void telemetry_callback(char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "📈 <b>Son %d Telemetri</b>\\n",
                     s_bridge.history_count);
    for (int i = 0; i < s_bridge.history_count && (size_t)n < buflen - 100; i++) {
        int idx = (s_bridge.history_idx - 1 - i + TELEMETRY_HISTORY) % TELEMETRY_HISTORY;
        /* Just send a shortened version */
        n += snprintf(buf + n, buflen - n, "\\n#%d: %-.80s...", i + 1,
                      s_bridge.telemetry_history[idx]);
    }
}

/* ── Bridge task ──────────────────────────────────────────────────────── */

static void bridge_task(void *arg)
{
    ESP_LOGI(TAG, "Bridge task started — polling S3 every %ds.", S3_POLL_INTERVAL_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(8000)); /* wait for WiFi to connect */

    while (true) {
        if (!wifi_dual_is_on_5g()) {
            poll_s3_telemetry();
        }
        vTaskDelay(pdMS_TO_TICKS(S3_POLL_INTERVAL_MS));
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t espnow_bridge_init(void)
{
    memset(&s_bridge, 0, sizeof(s_bridge));

    /* Register Telegram callbacks */
    telegram_register_status_cb(status_callback);
    telegram_register_telemetry_cb(telemetry_callback);

    xTaskCreate(bridge_task, "bridge", 6144, NULL, 3, NULL);

    ESP_LOGI(TAG, "ESP-NOW bridge initialized (S3 @ %s:%d)",
             CONFIG_SCOUT_S3_IP, CONFIG_SCOUT_S3_HTTP_PORT);
    return ESP_OK;
}

bool espnow_bridge_is_s3_online(void) { return s_bridge.s3_online; }
bool espnow_bridge_alarm_active(void) { return s_bridge.alarm_active; }
const char *espnow_bridge_alarm_reason(void) { return s_bridge.alarm_reason; }
