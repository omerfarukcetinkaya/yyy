/**
 * @file sensor_registry.c
 * @brief Multi-sensor polling + aggregated status (for heartbeat + /status).
 *
 * Replaces the hard-coded S3 polling in espnow_bridge. S3 is registered
 * as sensor 0; new sensors are added by appending to s_sensors[] below.
 *
 * Poll cycle: every SENSOR_POLL_INTERVAL_MS, iterate sensors and HTTP
 * GET each one's telemetry endpoint (only while on 2.4G band — 5G is
 * reserved for Telegram). Parse minimal JSON for common fields.
 *
 * On alarm state transitions (any sensor), relay to Telegram.
 */
#include "sensor_registry.h"
#include "wifi_dual.h"
#include "telegram_client.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "sensor_reg";

#define SENSOR_POLL_INTERVAL_MS   10000   /* poll each sensor every 10s */
#define SENSOR_HTTP_TIMEOUT_MS     5000

/* Static registry — edit this table to add more sensors. */
static sensor_t s_sensors[SENSOR_MAX];
static int      s_sensor_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* Per-sensor previous alarm state for edge detection. */
static bool s_prev_alarm[SENSOR_MAX];
/* Per-sensor previous motion-detected state — C5 treats motion as an
 * alert condition (S3's alarm_engine only fires on gas/temp, so motion
 * detection never raises S3's alarm.active field). Scout is the network
 * policy engine and decides what's worth notifying about. */
static bool s_prev_motion[SENSOR_MAX];

/* ── HTTP response buffer ─────────────────────────────────────────────── */
static char s_http_buf[2560];
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

/* ── Tiny JSON value extractors (same idiom as before) ────────────────── */
static int json_int(const char *j, const char *key, int def)
{
    char k[64]; snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(j, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    return atoi(p);
}
static float json_float(const char *j, const char *key, float def)
{
    char k[64]; snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(j, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    return (float)atof(p);
}
static bool json_bool(const char *j, const char *key, bool def)
{
    char k[64]; snprintf(k, sizeof(k), "\"%s\":", key);
    const char *p = strstr(j, k);
    if (!p) return def;
    p += strlen(k);
    while (*p == ' ') p++;
    return (*p == 't');
}
static void json_str(const char *j, const char *key, char *out, int maxlen)
{
    char k[64]; snprintf(k, sizeof(k), "\"%s\":\"", key);
    const char *p = strstr(j, k);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(k);
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* ── Poll one sensor ──────────────────────────────────────────────────── */

static void poll_one(int idx)
{
    sensor_t *s = &s_sensors[idx];
    char url[128];
    snprintf(url, sizeof(url), "http://%s%s", s->ip, s->telemetry_path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_evt,
        .timeout_ms = SENSOR_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .username = s->username,
        .password = s->password,
        .auth_type = (s->username && s->password) ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
    };

    s_http_len = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s->last_poll_us = now;

    if (err == ESP_OK && status == 200 && s_http_len > 0) {
        s_http_buf[s_http_len] = '\0';
        s->online = true;
        s->last_success_us = now;
        s->poll_ok++;

        /* Parse common telemetry fields */
        s->uptime_s        = (uint32_t)json_int(s_http_buf, "uptime_s", 0);
        s->cam_fps         = (uint32_t)json_int(s_http_buf, "fps_1s", 0);
        s->motion_fps      = (uint32_t)json_int(s_http_buf, "motion_fps_1s", 0);
        s->motion_detected = json_bool(s_http_buf, "motion_detected", false);
        s->motion_score    = json_float(s_http_buf, "motion_score", 0.0f);
        s->alarm_active    = json_bool(s_http_buf, "active", false);
        json_str(s_http_buf, "last_reason", s->alarm_reason, sizeof(s->alarm_reason));
        s->heap_free_b     = (uint32_t)json_int(s_http_buf, "heap_free_b", 0);
        s->psram_free_b    = (uint32_t)json_int(s_http_buf, "psram_free_b", 0);
        s->rssi            = (int8_t)json_int(s_http_buf, "rssi_dbm", 0);
        s->cpu0_pct        = (uint8_t)json_int(s_http_buf, "core0_pct", 0);
        s->cpu1_pct        = (uint8_t)json_int(s_http_buf, "core1_pct", 0);

        /* Alarm edge detection (off→on) for S3's alarm field (gas/temp). */
        if (s->alarm_active && !s_prev_alarm[idx]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: %s",
                     s->name,
                     s->alarm_reason[0] ? s->alarm_reason : "Alarm triggered");
            telegram_send_alert(msg);
        }
        s_prev_alarm[idx] = s->alarm_active;

        /* Motion edge detection (off→on) — C5 policy: motion is an alert.
         * Synthesize an alarm event when motion starts (with a reason). */
        if (s->motion_detected && !s_prev_motion[idx]) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "%s: Motion detected (score %.2f, %lu fps)",
                     s->name, s->motion_score, (unsigned long)s->motion_fps);
            telegram_send_alert(msg);
            /* Set alarm_reason so LED + admin panel reflect the motion. */
            snprintf(s->alarm_reason, sizeof(s->alarm_reason),
                     "Motion (%.2f)", s->motion_score);
        }
        s_prev_motion[idx] = s->motion_detected;
    } else {
        s->online = false;
        s->poll_fail++;
        ESP_LOGW(TAG, "poll [%s] fail: err=%s status=%d",
                 s->name, esp_err_to_name(err), status);
    }
    xSemaphoreGive(s_mutex);
}

/* ── Polling task ─────────────────────────────────────────────────────── */

static void registry_task(void *arg)
{
    ESP_LOGI(TAG, "sensor_registry task started (%d sensors, poll %ds)",
             s_sensor_count, SENSOR_POLL_INTERVAL_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(8000));

    while (true) {
        /* Polling only makes sense on 2.4G — sensors live there. */
        if (!wifi_dual_is_on_5g() && wifi_dual_is_connected()) {
            for (int i = 0; i < s_sensor_count; i++) {
                poll_one(i);
                vTaskDelay(pdMS_TO_TICKS(200));  /* spread load */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t sensor_registry_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    memset(s_sensors, 0, sizeof(s_sensors));
    memset(s_prev_alarm, 0, sizeof(s_prev_alarm));

    /* ── Register sensors ──────────────────────────────────────────────
     * Add a new sensor by appending here. Fields:
     *   name, ip, telemetry_path, username, password
     */

    /* Sensor 0: S3 Vision Hub (yy1) */
    s_sensors[0].name           = "S3 Vision Hub";
    s_sensors[0].ip             = CONFIG_SCOUT_S3_IP;
    s_sensors[0].telemetry_path = "/api/telemetry";
    s_sensors[0].username       = CONFIG_SCOUT_S3_USER;
    s_sensors[0].password       = CONFIG_SCOUT_S3_PASS;

    s_sensor_count = 1;

    ESP_LOGI(TAG, "Sensor registry initialized with %d sensor(s):", s_sensor_count);
    for (int i = 0; i < s_sensor_count; i++) {
        ESP_LOGI(TAG, "  [%d] %s @ %s", i, s_sensors[i].name, s_sensors[i].ip);
    }
    return ESP_OK;
}

esp_err_t sensor_registry_start(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    /* 12KB stack: HTTP client + mbedtls TLS context + JSON parse + edge
     * detection logging. 6KB was overflowing (Stack protection fault). */
    BaseType_t r = xTaskCreate(registry_task, "sensor_reg", 12288, NULL, 3, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

int sensor_registry_count(void) { return s_sensor_count; }

int sensor_registry_online_count(void)
{
    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_sensor_count; i++) if (s_sensors[i].online) n++;
    xSemaphoreGive(s_mutex);
    return n;
}

const sensor_t *sensor_registry_get(int idx)
{
    if (idx < 0 || idx >= s_sensor_count) return NULL;
    return &s_sensors[idx];
}

bool sensor_registry_any_alarm(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool any = false;
    /* Alarm = hardware alarm (gas/temp) OR active motion detection.
     * Scout's definition of "something worth alerting about". */
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].online &&
            (s_sensors[i].alarm_active || s_sensors[i].motion_detected)) {
            any = true; break;
        }
    }
    xSemaphoreGive(s_mutex);
    return any;
}

const sensor_t *sensor_registry_first_alarm(void)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].online &&
            (s_sensors[i].alarm_active || s_sensors[i].motion_detected)) {
            return &s_sensors[i];
        }
    }
    return NULL;
}

size_t sensor_registry_format_line(int idx, char *buf, size_t buflen)
{
    if (idx < 0 || idx >= s_sensor_count) return 0;
    const sensor_t *s = &s_sensors[idx];
    int n;
    if (!s->online) {
        n = snprintf(buf, buflen, "🔴 %s OFFLINE (fails=%lu)",
                     s->name, (unsigned long)s->poll_fail);
    } else {
        const char *alarm = s->alarm_active ? "⚠️ ALARM" : "";
        if (s->cam_fps > 0) {
            n = snprintf(buf, buflen,
                "🟢 %s · up %lus · cam %lufps · motion %lufps (%.2f) · RSSI %d%s%s",
                s->name,
                (unsigned long)s->uptime_s,
                (unsigned long)s->cam_fps,
                (unsigned long)s->motion_fps,
                s->motion_score,
                (int)s->rssi,
                alarm[0] ? " · " : "", alarm);
        } else {
            n = snprintf(buf, buflen,
                "🟢 %s · up %lus · RSSI %d%s%s",
                s->name,
                (unsigned long)s->uptime_s,
                (int)s->rssi,
                alarm[0] ? " · " : "", alarm);
        }
    }
    return (n < 0) ? 0 : (size_t)n;
}

size_t sensor_registry_format_all(char *buf, size_t buflen)
{
    size_t used = 0;
    int n = snprintf(buf, buflen,
        "🌐 <b>Sensör Ağı</b>: %d/%d online\\n",
        sensor_registry_online_count(), s_sensor_count);
    if (n < 0 || (size_t)n >= buflen) return 0;
    used = (size_t)n;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_sensor_count && used < buflen - 80; i++) {
        char line[SENSOR_SUMMARY_MAX];
        sensor_registry_format_line(i, line, sizeof(line));
        int m = snprintf(buf + used, buflen - used, "├ [%d] %s\\n", i, line);
        if (m < 0 || used + m >= buflen) break;
        used += m;
        /* Sensor detail lines */
        const sensor_t *s = &s_sensors[i];
        if (s->online) {
            m = snprintf(buf + used, buflen - used,
                "│   polls %lu/%lu · heap %luKB",
                (unsigned long)s->poll_ok,
                (unsigned long)s->poll_fail,
                (unsigned long)(s->heap_free_b / 1024));
            if (m > 0 && used + m < buflen) used += m;
            if (s->cpu0_pct || s->cpu1_pct) {
                m = snprintf(buf + used, buflen - used,
                    " · CPU %u/%u%%",
                    (unsigned)s->cpu0_pct, (unsigned)s->cpu1_pct);
                if (m > 0 && used + m < buflen) used += m;
            }
            if (s->alarm_active && s->alarm_reason[0]) {
                m = snprintf(buf + used, buflen - used,
                    "\\n│   alarm: %s", s->alarm_reason);
                if (m > 0 && used + m < buflen) used += m;
            }
            if (used + 4 < buflen) {
                memcpy(buf + used, "\\n", 2);
                used += 2;
                buf[used] = '\0';
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return used;
}
