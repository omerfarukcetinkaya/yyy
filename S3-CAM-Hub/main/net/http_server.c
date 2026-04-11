/**
 * @file http_server.c
 * @brief Central HTTP server lifecycle and route registration.
 */
#include "http_server.h"
#include "stream_handler.h"
#include "admin_panel.h"
#include "web_reporter.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "http_srv";

static httpd_handle_t s_server = NULL;

/* Pre-registered URIs (populated by modules before server starts) */
#define MAX_URI_HANDLERS 16
static httpd_uri_t  s_uri_table[MAX_URI_HANDLERS];
static uint8_t      s_uri_count = 0;

esp_err_t http_server_register_uri(const httpd_uri_t *uri)
{
    if (!uri) return ESP_ERR_INVALID_ARG;
    if (s_uri_count >= MAX_URI_HANDLERS) {
        ESP_LOGE(TAG, "URI handler table full (%d entries).", MAX_URI_HANDLERS);
        return ESP_ERR_NO_MEM;
    }
    /* Register immediately if server is running */
    if (s_server) {
        esp_err_t ret = httpd_register_uri_handler(s_server, uri);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register URI '%s': %s", uri->uri, esp_err_to_name(ret));
            return ret;
        }
    }
    /* Store for re-registration after server restart */
    memcpy(&s_uri_table[s_uri_count], uri, sizeof(httpd_uri_t));
    s_uri_count++;
    return ESP_OK;
}

esp_err_t http_server_start(void)
{
    if (s_server) {
        ESP_LOGD(TAG, "Server already running.");
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port        = CONFIG_VH_WEB_PORT;
    cfg.max_open_sockets   = 13;  /* 2 clients × (page+stream+telemetry) + headroom */
    cfg.lru_purge_enable   = true;
    cfg.stack_size         = 8192;
    cfg.core_id            = 0;   /* HTTP server workers on Core 0 */
    cfg.task_priority      = 6;
    /* iOS Safari shows a "Reduce Protections" security dialog before sending
     * the HTTP request. The dialog can take minutes to dismiss. Default
     * recv_wait_timeout=5s closes the connection before the request arrives.
     * 60 s keeps the connection alive through the dialog. */
    cfg.recv_wait_timeout  = 60;  /* seconds, default=5 — survives iOS "Reduce Protections" dialog */
    cfg.send_wait_timeout  = 5;   /* seconds, default=5 — keep at default; 30s was freezing the httpd task */

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start FAILED: %s  (max_open_sockets=%d — check CONFIG_LWIP_MAX_SOCKETS)",
                 esp_err_to_name(ret), cfg.max_open_sockets);
        return ret;
    }

    ESP_LOGI(TAG, "HTTP server started: port=%d  max_sockets=%d  stack=%lu  core=%d",
             CONFIG_VH_WEB_PORT, cfg.max_open_sockets,
             (unsigned long)cfg.stack_size, cfg.core_id);

    /* Register all deferred URI handlers */
    for (uint8_t i = 0; i < s_uri_count; i++) {
        ret = httpd_register_uri_handler(s_server, &s_uri_table[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "URI '%s' registration failed: %s",
                     s_uri_table[i].uri, esp_err_to_name(ret));
        }
    }

    /* Register fresh routes from each module */
    stream_handler_register(s_server);
    admin_panel_register(s_server);
    web_reporter_register(s_server);

    return ESP_OK;
}

void http_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped.");
}

bool http_server_is_running(void)
{
    return s_server != NULL;
}

httpd_handle_t http_server_get_handle(void)
{
    return s_server;
}
