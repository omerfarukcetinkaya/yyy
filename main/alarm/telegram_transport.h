/**
 * @file telegram_transport.h
 * @brief Telegram Bot API transport for the alert system.
 *
 * Phase 1: STUB — logs the alert but does not send HTTP requests.
 * Phase 5: Implement real HTTPS POST to api.telegram.org/bot<token>/sendMessage
 *
 * Requires VH_TELEGRAM_ENABLED=y and VH_TELEGRAM_BOT_TOKEN / VH_TELEGRAM_CHAT_ID
 * to be set in menuconfig.
 */
#pragma once
#include "alert_transport.h"

/**
 * @brief Get a pointer to the Telegram transport instance.
 * Returns NULL if VH_TELEGRAM_ENABLED is not set.
 */
const alert_transport_t *telegram_transport_get(void);
