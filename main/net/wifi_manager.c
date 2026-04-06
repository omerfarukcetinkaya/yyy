/**
 * @file wifi_manager.c
 * @brief Wi-Fi station manager with automatic reconnection.
 */
#include "wifi_manager.h"
#include "http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_event_group = NULL;
static char    s_ip[16]         = "0.0.0.0";
static int8_t  s_rssi           = 0;
static uint32_t s_reconnect_cnt = 0;
static bool    s_initialized    = false;

/* Reconnect delay and max retries from Kconfig */
#define RECONNECT_DELAY_MS  CONFIG_VH_WIFI_RECONNECT_DELAY_MS
#define MAX_RETRY           CONFIG_VH_WIFI_MAX_RETRY

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting to '%s' ...", CONFIG_VH_WIFI_SSID);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            xEventGroupSetBits(s_event_group, WIFI_BIT_CONNECTED);
            /* Get RSSI */
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                s_rssi = ap_info.rssi;
            }
            ESP_LOGI(TAG, "Connected to AP '%s', RSSI: %d dBm", CONFIG_VH_WIFI_SSID, s_rssi);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = event_data;
            xEventGroupClearBits(s_event_group, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
            strncpy(s_ip, "0.0.0.0", sizeof(s_ip));
            s_rssi = 0;

            s_reconnect_cnt++;
            ESP_LOGW(TAG, "Disconnected (reason=%d). Reconnect #%lu in %d ms ...",
                     ev->reason, (unsigned long)s_reconnect_cnt, RECONNECT_DELAY_MS);

            if (s_reconnect_cnt >= MAX_RETRY) {
                ESP_LOGE(TAG, "Max retries reached (%d). Rebooting.", MAX_RETRY);
                esp_restart();
            }

            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            esp_wifi_connect();
            break;
        }

        default:
            break;
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_reconnect_cnt = 0; /* Reset on successful connect */

        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        xEventGroupSetBits(s_event_group, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);

        /* Start HTTP server (idempotent on re-connect) */
        http_server_start();
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    s_event_group = xEventGroupCreate();
    if (!s_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    /* Configure station */
    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_VH_WIFI_SSID,
            .password = CONFIG_VH_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi manager initialized. Target SSID: '%s'", CONFIG_VH_WIFI_SSID);
    return ESP_OK;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_event_group;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!s_initialized) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_rssi = ap_info.rssi;
    }
    return s_rssi;
}

uint32_t wifi_manager_get_reconnect_count(void)
{
    return s_reconnect_cnt;
}

bool wifi_manager_is_connected(void)
{
    if (!s_event_group) return false;
    return (xEventGroupGetBits(s_event_group) & WIFI_BIT_GOT_IP) != 0;
}
