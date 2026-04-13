/**
 * @file telegram_client.c
 * @brief HTTPS POST to Telegram Bot API for alarm relay.
 * STUB: will implement HTTPS client with bot token.
 */
#include "telegram_client.h"
#include "esp_log.h"

static const char *TAG = "telegram";

esp_err_t telegram_client_init(void)
{
    ESP_LOGI(TAG, "Telegram client init (STUB).");
    return ESP_OK;
}

esp_err_t telegram_send_alert(const char *message)
{
    ESP_LOGI(TAG, "Telegram alert: %s (STUB — not sent).", message);
    return ESP_OK;
}
