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
#define BIST_INTERVAL_MS    (15 * 60 * 1000)   /* 15 min heartbeat/BIST */

/* ── Parsed S3 telemetry fields (accumulated for BIST) ─────────────── */
typedef struct {
    uint32_t uptime_s;
    uint32_t cam_fps;
    uint32_t cam_frames;
    uint32_t cam_drops;
    uint32_t motion_fps;
    bool     motion_detected;
    float    motion_score;
    uint32_t stream_clients;
    uint8_t  cpu_core0;
    uint8_t  cpu_core1;
    uint32_t heap_free;
    uint32_t psram_free;
    int8_t   wifi_rssi;
    bool     alarm_active;
    char     alarm_reason[64];
} s3_snapshot_t;

static struct {
    char    telemetry_history[TELEMETRY_HISTORY][512];
    int     history_idx;
    int     history_count;
    bool    s3_online;
    int64_t last_s3_poll_us;
    /* Last alarm info */
    bool    alarm_active;
    char    alarm_reason[128];
    /* BIST accumulation (15-min window) */
    s3_snapshot_t last_snap;        /* most recent parsed snapshot */
    uint32_t bist_polls_ok;         /* successful polls in window */
    uint32_t bist_polls_fail;       /* failed polls */
    uint32_t bist_alarm_count;      /* alarm triggers in window */
    uint32_t bist_motion_detects;   /* motion detection events */
    float    bist_max_motion_score;
    uint32_t bist_min_cam_fps;
    uint32_t bist_max_cam_fps;
    int64_t  bist_last_report_us;
} s_bridge;

/* ── Simple JSON value extractor ────────────────────────────────────────── */

static int json_int(const char *json, const char *key, int def)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

static float json_float(const char *json, const char *key, float def)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    return (float)atof(p);
}

static bool json_bool(const char *json, const char *key, bool def)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    return (*p == 't');
}

static void json_str(const char *json, const char *key, char *out, int maxlen)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* Parse S3 /api/telemetry JSON into snapshot struct */
static void parse_s3_telemetry(const char *json, s3_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->uptime_s        = (uint32_t)json_int(json, "uptime_s", 0);
    snap->cam_fps         = (uint32_t)json_int(json, "fps_1s", 0);
    snap->cam_frames      = (uint32_t)json_int(json, "frame_count", 0);
    snap->cam_drops       = (uint32_t)json_int(json, "drop_count", 0);
    snap->motion_fps      = (uint32_t)json_int(json, "motion_fps_1s", 0);
    snap->motion_detected = json_bool(json, "motion_detected", false);
    snap->motion_score    = json_float(json, "motion_score", 0.0f);
    snap->stream_clients  = (uint32_t)json_int(json, "client_count", 0);
    snap->cpu_core0       = (uint8_t)json_int(json, "core0_pct", 0);
    snap->cpu_core1       = (uint8_t)json_int(json, "core1_pct", 0);
    snap->heap_free       = (uint32_t)json_int(json, "heap_free_b", 0);
    snap->psram_free      = (uint32_t)json_int(json, "psram_free_b", 0);
    snap->wifi_rssi       = (int8_t)json_int(json, "rssi_dbm", 0);
    snap->alarm_active    = json_bool(json, "active", false);
    json_str(json, "last_reason", snap->alarm_reason, sizeof(snap->alarm_reason));
}

/* ── BIST (Built-In Self Test) 15-minute report ────────────────────────── */

static void send_bist_report(void)
{
    s3_snapshot_t *s = &s_bridge.last_snap;
    char buf1[1600], buf2[1200];

    /* Message 1: Heartbeat + Scout status */
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);

    snprintf(buf1, sizeof(buf1),
        "💓 <b>Scout Heartbeat</b> %02d:%02d:%02d\\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━\\n"
        "\\n"
        "🛡 <b>Scout C5 Status</b>\\n"
        "├ WiFi: %s [%s] RSSI %d dBm\\n"
        "├ Uptime: %lu s\\n"
        "├ Heap free: %lu B\\n"
        "├ S3 polls OK/FAIL: %lu/%lu\\n"
        "├ Telegram: %s\\n"
        "└ Alarms relayed: %lu\\n"
        "\\n"
        "📹 <b>S3 Vision Hub</b>\\n"
        "├ Status: %s\\n"
        "├ S3 Uptime: %lu s\\n"
        "├ Camera: %lu fps (%lu frames, %lu drops)\\n"
        "├ FPS range: %lu–%lu (15min)\\n"
        "├ Motion: %lu fps (detected=%s score=%.3f)\\n"
        "├ Motion events (15min): %lu (max score: %.3f)\\n"
        "├ Stream clients: %lu\\n"
        "├ CPU: Core0=%u%% Core1=%u%%\\n"
        "├ Heap: %lu KB free\\n"
        "├ PSRAM: %lu KB free\\n"
        "└ WiFi RSSI: %d dBm",
        ti.tm_hour, ti.tm_min, ti.tm_sec,
        wifi_dual_is_connected() ? "UP" : "DOWN",
        wifi_dual_is_on_5g() ? "5G" : "2.4G",
        (int)wifi_dual_get_rssi(),
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)s_bridge.bist_polls_ok,
        (unsigned long)s_bridge.bist_polls_fail,
        telegram_is_muted() ? "MUTED 🔇" : "Active 🔊",
        (unsigned long)s_bridge.bist_alarm_count,
        s_bridge.s3_online ? "🟢 ONLINE" : "🔴 OFFLINE",
        (unsigned long)s->uptime_s,
        (unsigned long)s->cam_fps,
        (unsigned long)s->cam_frames,
        (unsigned long)s->cam_drops,
        (unsigned long)s_bridge.bist_min_cam_fps,
        (unsigned long)s_bridge.bist_max_cam_fps,
        (unsigned long)s->motion_fps,
        s->motion_detected ? "YES" : "no",
        s->motion_score,
        (unsigned long)s_bridge.bist_motion_detects,
        s_bridge.bist_max_motion_score,
        (unsigned long)s->stream_clients,
        (unsigned)s->cpu_core0, (unsigned)s->cpu_core1,
        (unsigned long)(s->heap_free / 1024),
        (unsigned long)(s->psram_free / 1024),
        (int)s->wifi_rssi);

    /* Message 2: Alarm summary */
    snprintf(buf2, sizeof(buf2),
        "🚨 <b>Alarm Status</b>\\n"
        "├ Current: %s\\n"
        "├ Reason: %s\\n"
        "├ Triggers (15min): %lu\\n"
        "└ Mute: %s\\n"
        "\\n"
        "📊 <b>15-min Summary</b>\\n"
        "├ Polls: %lu OK / %lu failed\\n"
        "├ S3 availability: %s\\n"
        "├ Motion events: %lu\\n"
        "└ Next report in 15 min",
        s->alarm_active ? "⚠️ ACTIVE" : "✅ Clear",
        s->alarm_active ? s->alarm_reason : "—",
        (unsigned long)s_bridge.bist_alarm_count,
        telegram_is_muted() ? "ON (susturulmuş)" : "OFF (aktif)",
        (unsigned long)s_bridge.bist_polls_ok,
        (unsigned long)s_bridge.bist_polls_fail,
        (s_bridge.bist_polls_ok > 0) ? "OK" : "UNREACHABLE",
        (unsigned long)s_bridge.bist_motion_detects);

    /* Send both messages */
    telegram_send(buf1);
    vTaskDelay(pdMS_TO_TICKS(1000));  /* avoid rate limit */
    telegram_send(buf2);

    ESP_LOGI(TAG, "BIST report sent (polls=%lu/%lu alarms=%lu motions=%lu)",
             (unsigned long)s_bridge.bist_polls_ok,
             (unsigned long)s_bridge.bist_polls_fail,
             (unsigned long)s_bridge.bist_alarm_count,
             (unsigned long)s_bridge.bist_motion_detects);

    /* Reset BIST counters for next window */
    s_bridge.bist_polls_ok = 0;
    s_bridge.bist_polls_fail = 0;
    s_bridge.bist_alarm_count = 0;
    s_bridge.bist_motion_detects = 0;
    s_bridge.bist_max_motion_score = 0;
    s_bridge.bist_min_cam_fps = 999;
    s_bridge.bist_max_cam_fps = 0;
    s_bridge.bist_last_report_us = esp_timer_get_time();
}

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
        s_bridge.bist_polls_ok++;

        /* Store in history ring */
        int idx = s_bridge.history_idx % TELEMETRY_HISTORY;
        strncpy(s_bridge.telemetry_history[idx], s_http_buf,
                sizeof(s_bridge.telemetry_history[idx]) - 1);
        s_bridge.history_idx++;
        if (s_bridge.history_count < TELEMETRY_HISTORY)
            s_bridge.history_count++;

        /* Parse full telemetry into snapshot */
        parse_s3_telemetry(s_http_buf, &s_bridge.last_snap);
        s3_snapshot_t *snap = &s_bridge.last_snap;

        /* BIST accumulation */
        if (snap->cam_fps < s_bridge.bist_min_cam_fps)
            s_bridge.bist_min_cam_fps = snap->cam_fps;
        if (snap->cam_fps > s_bridge.bist_max_cam_fps)
            s_bridge.bist_max_cam_fps = snap->cam_fps;
        if (snap->motion_detected) {
            s_bridge.bist_motion_detects++;
            if (snap->motion_score > s_bridge.bist_max_motion_score)
                s_bridge.bist_max_motion_score = snap->motion_score;
        }

        /* Check for alarm state change */
        bool alarm_active = snap->alarm_active;
        if (alarm_active && !s_bridge.alarm_active) {
            s_bridge.bist_alarm_count++;
            const char *reason = snap->alarm_reason[0] ? snap->alarm_reason : "Motion detected";
            strncpy(s_bridge.alarm_reason, reason, sizeof(s_bridge.alarm_reason) - 1);

            char alert_msg[256];
            snprintf(alert_msg, sizeof(alert_msg),
                     "S3 Vision Hub: %s", reason);
            telegram_send_alert(alert_msg);
        }
        s_bridge.alarm_active = alarm_active;

        ESP_LOGD(TAG, "S3 telemetry polled OK (%d bytes) cam=%lu fps alarm=%s",
                 s_http_len, (unsigned long)snap->cam_fps,
                 alarm_active ? "YES" : "no");
    } else {
        s_bridge.s3_online = false;
        s_bridge.bist_polls_fail++;
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
    ESP_LOGI(TAG, "Bridge task started — S3 poll every %ds, BIST every %d min.",
             S3_POLL_INTERVAL_MS / 1000, BIST_INTERVAL_MS / 60000);
    vTaskDelay(pdMS_TO_TICKS(8000));

    s_bridge.bist_last_report_us = esp_timer_get_time();
    s_bridge.bist_min_cam_fps = 999;

    while (true) {
        /* Poll S3 when on 2.4G */
        if (!wifi_dual_is_on_5g()) {
            poll_s3_telemetry();
        }

        /* 15-minute BIST heartbeat */
        int64_t now_us = esp_timer_get_time();
        if ((now_us - s_bridge.bist_last_report_us) >= (int64_t)BIST_INTERVAL_MS * 1000LL) {
            ESP_LOGI(TAG, "BIST interval reached — sending report...");
            send_bist_report();
        }

        vTaskDelay(pdMS_TO_TICKS(S3_POLL_INTERVAL_MS));
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t espnow_bridge_init(void)
{
    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.bist_min_cam_fps = 999;
    s_bridge.bist_last_report_us = esp_timer_get_time();

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
