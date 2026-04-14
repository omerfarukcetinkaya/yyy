/**
 * @file status_reporter.c
 * @brief Telemetry HTTP server on 2.4G network + UART reporting.
 *
 * Serves GET /api/status on port 8080 (CONFIG_SCOUT_TELEMETRY_PORT)
 * when connected to 2.4G. Prints status to UART every 10 seconds.
 */
#include "status_reporter.h"
#include "net/wifi_dual.h"
#include "net/telegram_client.h"
#include "net/espnow_bridge.h"
#include "system/watchdog.h"
#include "system/rgb_led.h"
#include "net/admin_panel.h"
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "status_rpt";

static httpd_handle_t s_server = NULL;

static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"device\":\"scout-c5\","
        "\"wifi\":{\"connected\":%s,\"band\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
        "\"s3\":{\"online\":%s,\"alarm\":%s,\"alarm_reason\":\"%s\"},"
        "\"telegram\":{\"muted\":%s},"
        "\"system\":{\"uptime_s\":%lu,\"heap_free\":%lu}"
        "}",
        wifi_dual_is_connected() ? "true" : "false",
        wifi_dual_is_on_5g() ? "5G" : "2.4G",
        wifi_dual_get_ip(),
        (int)wifi_dual_get_rssi(),
        espnow_bridge_is_s3_online() ? "true" : "false",
        espnow_bridge_alarm_active() ? "true" : "false",
        espnow_bridge_alarm_reason(),
        telegram_is_muted() ? "true" : "false",
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static void start_http_server(void)
{
    if (s_server) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_SCOUT_TELEMETRY_PORT;
    cfg.max_open_sockets = 4;

    if (httpd_start(&s_server, &cfg) == ESP_OK) {
        httpd_uri_t uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = status_handler,
        };
        httpd_register_uri_handler(s_server, &uri);
        scout_admin_register(s_server);
        ESP_LOGI(TAG, "HTTP server on port %d (admin + telemetry API)",
                 CONFIG_SCOUT_TELEMETRY_PORT);
    }
}

static void reporter_task(void *arg)
{
    ESP_LOGI(TAG, "Status reporter started.");

    /* Wait for WiFi on 2.4G before starting HTTP server.
     * Starting it mid band-switch causes it to bind to the wrong netif
     * and become unreachable from 2.4G clients. */
    for (int i = 0; i < 60; i++) {
        if (wifi_dual_is_connected() && !wifi_dual_is_on_5g()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); /* extra settle */

    start_http_server();

    while (true) {
        bool alarm = espnow_bridge_alarm_active();

        /* RGB LED: red on alarm, off otherwise */
        rgb_led_alarm(alarm);

        ESP_LOGI(TAG, "======== SCOUT ========");
        ESP_LOGI(TAG, "[WIFI] %s [%s] RSSI=%d IP=%s",
                 wifi_dual_is_connected() ? "UP" : "DOWN",
                 wifi_dual_is_on_5g() ? "5G" : "2.4G",
                 (int)wifi_dual_get_rssi(),
                 wifi_dual_get_ip());
        ESP_LOGI(TAG, "[S3]   %s  alarm=%s",
                 espnow_bridge_is_s3_online() ? "ONLINE" : "OFFLINE",
                 alarm ? espnow_bridge_alarm_reason() : "clear");
        ESP_LOGI(TAG, "[TG]   muted=%s", telegram_is_muted() ? "yes" : "no");
        ESP_LOGI(TAG, "[MEM]  heap=%lu B", (unsigned long)esp_get_free_heap_size());
        ESP_LOGI(TAG, "[TIME] uptime=%lu s",
                 (unsigned long)(esp_timer_get_time() / 1000000));
        ESP_LOGI(TAG, "=======================");

        watchdog_reset();
        vTaskDelay(pdMS_TO_TICKS(2000));  /* 2s UART telemetry */
    }
}

esp_err_t status_reporter_init(void) { return ESP_OK; }

esp_err_t status_reporter_start(void)
{
    BaseType_t r = xTaskCreate(reporter_task, "status_rpt", 4096, NULL, 2, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}
