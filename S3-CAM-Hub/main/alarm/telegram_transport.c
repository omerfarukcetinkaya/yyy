/**
 * @file telegram_transport.c
 * @brief Telegram transport — Phase 1 STUB.
 *
 * TODO (Phase 5):
 * 1. Use esp_http_client to POST to:
 *    https://api.telegram.org/bot<VH_TELEGRAM_BOT_TOKEN>/sendMessage
 *    Body: {"chat_id": "<VH_TELEGRAM_CHAT_ID>", "text": "<message>"}
 * 2. Handle HTTP response code (200=ok, 4xx=config error, 5xx=server error)
 * 3. Implement retry with backoff on transient failures
 * 4. Track sent/fail counters in telemetry
 */
#include "telegram_transport.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>

/* All functions and the transport struct are compiled only when enabled.
 * When disabled, only telegram_transport_get() (returning NULL) is compiled.
 * This avoids -Wunused-function warnings for stub functions. */
#ifdef CONFIG_VH_TELEGRAM_ENABLED

static const char *TAG = "telegram";

static esp_err_t telegram_init(void)
{
    ESP_LOGI(TAG, "Telegram transport initialized (STUB). Token set: %s",
             strlen(CONFIG_VH_TELEGRAM_BOT_TOKEN) > 0 ? "yes" : "NO");
    return ESP_OK;
}

static esp_err_t telegram_send(const alert_msg_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    /* TODO Phase 5: real HTTPS POST to api.telegram.org */
    ESP_LOGW(TAG, "[STUB] Would send Telegram: severity=%d title=%s",
             msg->severity, msg->title);
    ESP_LOGW(TAG, "[STUB] body: %s", msg->body);
    return ESP_OK;
}

static const alert_transport_t s_telegram_transport = {
    .name = "telegram",
    .init = telegram_init,
    .send = telegram_send,
};

#endif /* CONFIG_VH_TELEGRAM_ENABLED */

const alert_transport_t *telegram_transport_get(void)
{
#ifdef CONFIG_VH_TELEGRAM_ENABLED
    return &s_telegram_transport;
#else
    return NULL;
#endif
}
