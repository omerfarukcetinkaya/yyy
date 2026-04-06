/**
 * @file alert_transport.h
 * @brief Abstract alert transport interface.
 *
 * Any concrete transport (Telegram, MQTT, email) implements this interface.
 * The alarm engine holds a pointer to an alert_transport_t and calls
 * send() without knowing the transport details.
 *
 * To add a new transport: implement alert_transport_t and register it via
 * alarm_engine_set_transport().
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Alert severity level.
 */
typedef enum {
    ALERT_SEV_INFO    = 0,
    ALERT_SEV_WARNING = 1,
    ALERT_SEV_CRITICAL = 2,
} alert_severity_t;

/**
 * @brief An alert message payload.
 */
typedef struct {
    alert_severity_t severity;
    char             title[64];
    char             body[256];
    int64_t          timestamp_us;
} alert_msg_t;

/**
 * @brief Transport interface. Each concrete transport fills this struct.
 */
typedef struct {
    /** Human-readable name, e.g. "telegram", "mqtt". */
    const char *name;

    /**
     * @brief Send an alert.
     * @return ESP_OK if delivered, ESP_FAIL otherwise.
     */
    esp_err_t (*send)(const alert_msg_t *msg);

    /**
     * @brief Optional: initialize the transport (e.g. check connectivity).
     * May be NULL.
     */
    esp_err_t (*init)(void);
} alert_transport_t;
