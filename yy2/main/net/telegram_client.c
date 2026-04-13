/**
 * @file telegram_client.c
 * @brief Telegram Bot API client — alarm relay + keyword command handler.
 *
 * Sends alerts to yyy group / SecBridge topic.
 * Polls for incoming messages and parses keywords:
 *   /status     — reply with system status summary
 *   /mute       — silence alarms until next schedule start
 *   /unmute     — reactivate alarms immediately
 *   /camera     — (future) request camera stream VPN link
 *   /alarm      — force a test alarm
 *   /telemetry  — send last 5 health snapshots
 *   /help       — list available commands
 *
 * Uses esp_http_client for HTTPS POST to api.telegram.org.
 * Switches to 5G band before sending, releases after.
 */
#include "telegram_client.h"
#include "wifi_dual.h"
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
#include <time.h>

static const char *TAG = "telegram";

#define TG_API_BASE     "https://api.telegram.org/bot" CONFIG_SCOUT_TG_BOT_TOKEN
#define TG_SEND_URL     TG_API_BASE "/sendMessage"
#define TG_UPDATES_URL  TG_API_BASE "/getUpdates"
#define GROUP_ID        CONFIG_SCOUT_TG_GROUP_ID
#define THREAD_ID_STR   "2"     /* SecBridge topic */

#define POLL_INTERVAL_MS    5000
#define SEND_TIMEOUT_MS     15000
#define MAX_MSG_LEN         2048
#define MAX_RESPONSE_BUF    4096

static struct {
    bool        initialized;
    bool        muted;         /* alarm mute state */
    int64_t     mute_until_us; /* mute expiry (0 = until next schedule) */
    int         last_update_id;
    SemaphoreHandle_t send_mutex;
    char        response_buf[MAX_RESPONSE_BUF];
    int         response_len;
    /* Callback for status/telemetry queries */
    telegram_status_cb_t status_cb;
    telegram_telemetry_cb_t telemetry_cb;
} s_tg;

/* ── HTTP event handler ──────────────────────────────────────────────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_tg.response_len + evt->data_len < MAX_RESPONSE_BUF) {
            memcpy(s_tg.response_buf + s_tg.response_len, evt->data, evt->data_len);
            s_tg.response_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── Send message ────────────────────────────────────────────────────── */

esp_err_t telegram_send(const char *text)
{
    if (!s_tg.initialized || !text) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_tg.send_mutex, portMAX_DELAY);

    /* Switch to 5G for internet */
    wifi_dual_switch_to_5g();
    vTaskDelay(pdMS_TO_TICKS(500)); /* let connection stabilize */

    char post_data[MAX_MSG_LEN + 256];
    int n = snprintf(post_data, sizeof(post_data),
        "{\"chat_id\":\"%s\",\"message_thread_id\":%s,"
        "\"text\":\"%s\",\"parse_mode\":\"HTML\"}",
        GROUP_ID, THREAD_ID_STR, text);

    esp_http_client_config_t cfg = {
        .url = TG_SEND_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = SEND_TIMEOUT_MS,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, n);

    s_tg.response_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    wifi_dual_release_lock();

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Send failed: err=%s status=%d", esp_err_to_name(err), status);
    } else {
        ESP_LOGI(TAG, "Message sent to SecBridge (%d bytes)", n);
    }

    xSemaphoreGive(s_tg.send_mutex);
    return err;
}

esp_err_t telegram_send_alert(const char *message)
{
    if (s_tg.muted) {
        ESP_LOGI(TAG, "Alert muted: %s", message);
        return ESP_OK;
    }

    /* Check schedule: 09:00 - 19:00 */
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    int hour = timeinfo.tm_hour;
    if (hour < CONFIG_SCOUT_SCHEDULE_START_HOUR ||
        hour >= CONFIG_SCOUT_SCHEDULE_END_HOUR) {
        ESP_LOGD(TAG, "Outside schedule (%02d:00), alert suppressed.", hour);
        return ESP_OK;
    }

    char buf[MAX_MSG_LEN];
    snprintf(buf, sizeof(buf), "🚨 <b>ALARM</b>\\n%s", message);
    return telegram_send(buf);
}

/* ── Poll for incoming commands ──────────────────────────────────────── */

static void process_command(const char *text)
{
    if (!text) return;
    ESP_LOGI(TAG, "Command received: %s", text);

    if (strstr(text, "/mute") || strstr(text, "alarm sustur")) {
        s_tg.muted = true;
        telegram_send("🔇 Alarmlar susturuldu. Yarın sabah otomatik açılacak.");
    }
    else if (strstr(text, "/unmute") || strstr(text, "alarm ac") ||
             strstr(text, "alarm aç")) {
        s_tg.muted = false;
        telegram_send("🔊 Alarmlar tekrar aktif.");
    }
    else if (strstr(text, "/status") || strstr(text, "durum")) {
        if (s_tg.status_cb) {
            char buf[1024];
            s_tg.status_cb(buf, sizeof(buf));
            telegram_send(buf);
        } else {
            telegram_send("📊 Status callback not registered.");
        }
    }
    else if (strstr(text, "/telemetry") || strstr(text, "telemetri")) {
        if (s_tg.telemetry_cb) {
            char buf[2048];
            s_tg.telemetry_cb(buf, sizeof(buf));
            telegram_send(buf);
        } else {
            telegram_send("📈 Telemetry callback not registered.");
        }
    }
    else if (strstr(text, "/alarm") || strstr(text, "test alarm")) {
        telegram_send("🚨 <b>TEST ALARM</b>\\nThis is a test alarm from SecBridge.");
    }
    else if (strstr(text, "/camera") || strstr(text, "kamera")) {
        telegram_send("📷 Kamera erişimi henüz aktif değil (VPN entegrasyonu bekliyor).");
    }
    else if (strstr(text, "/help") || strstr(text, "yardim") ||
             strstr(text, "yardım") || strstr(text, "komutlar")) {
        telegram_send(
            "🛡 <b>SecBridge Komutları</b>\\n\\n"
            "/status — Sistem durumu\\n"
            "/mute — Alarmları sustur\\n"
            "/unmute — Alarmları aç\\n"
            "/alarm — Test alarm\\n"
            "/camera — Kamera erişimi\\n"
            "/telemetry — Son 5 sağlık raporu\\n"
            "/help — Bu mesaj"
        );
    }
}

/* Simple JSON string value extractor (no full parser needed) */
static bool json_get_string(const char *json, const char *key, char *out, int maxlen)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        /* Try without quotes for numbers */
        snprintf(search, sizeof(search), "\"%s\":", key);
        p = strstr(json, search);
        if (!p) return false;
        p += strlen(search);
        int i = 0;
        while (*p && *p != ',' && *p != '}' && i < maxlen - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return i > 0;
    }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static void poll_updates(void)
{
    wifi_dual_switch_to_5g();
    vTaskDelay(pdMS_TO_TICKS(300));

    char url[256];
    snprintf(url, sizeof(url), "%s?offset=%d&timeout=3&allowed_updates=[\"message\"]",
             TG_UPDATES_URL, s_tg.last_update_id + 1);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    s_tg.response_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    wifi_dual_release_lock();

    if (err != ESP_OK || s_tg.response_len == 0) return;
    s_tg.response_buf[s_tg.response_len] = '\0';

    /* Parse updates — look for messages in our thread */
    char *p = s_tg.response_buf;
    while ((p = strstr(p, "\"update_id\":")) != NULL) {
        char uid_str[16] = {0};
        json_get_string(p, "update_id", uid_str, sizeof(uid_str));
        int uid = atoi(uid_str);
        if (uid > s_tg.last_update_id) {
            s_tg.last_update_id = uid;
        }

        /* Check if message is in our thread */
        char thread_str[16] = {0};
        if (json_get_string(p, "message_thread_id", thread_str, sizeof(thread_str))) {
            if (atoi(thread_str) == CONFIG_SCOUT_TG_THREAD_ID) {
                char text[256] = {0};
                if (json_get_string(p, "text", text, sizeof(text))) {
                    process_command(text);
                }
            }
        }
        p++; /* advance past current match */
    }
}

/* ── Telegram polling task ───────────────────────────────────────────── */

static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Telegram poll task started.");
    vTaskDelay(pdMS_TO_TICKS(10000)); /* wait for WiFi */

    while (true) {
        poll_updates();

        /* Auto-unmute at schedule start each day */
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        if (s_tg.muted && timeinfo.tm_hour == CONFIG_SCOUT_SCHEDULE_START_HOUR &&
            timeinfo.tm_min == 0) {
            s_tg.muted = false;
            telegram_send("🔊 Yeni gün — alarmlar otomatik açıldı.");
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t telegram_client_init(void)
{
    memset(&s_tg, 0, sizeof(s_tg));
    s_tg.send_mutex = xSemaphoreCreateMutex();
    if (!s_tg.send_mutex) return ESP_ERR_NO_MEM;
    s_tg.last_update_id = 0;
    s_tg.initialized = true;

    xTaskCreate(telegram_poll_task, "tg_poll", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "Telegram client initialized (group=%s thread=%d schedule=%d-%d)",
             GROUP_ID, CONFIG_SCOUT_TG_THREAD_ID,
             CONFIG_SCOUT_SCHEDULE_START_HOUR, CONFIG_SCOUT_SCHEDULE_END_HOUR);
    return ESP_OK;
}

void telegram_register_status_cb(telegram_status_cb_t cb)
{
    s_tg.status_cb = cb;
}

void telegram_register_telemetry_cb(telegram_telemetry_cb_t cb)
{
    s_tg.telemetry_cb = cb;
}

bool telegram_is_muted(void) { return s_tg.muted; }
