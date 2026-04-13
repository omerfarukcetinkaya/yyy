#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef void (*telegram_status_cb_t)(char *buf, size_t buflen);
typedef void (*telegram_telemetry_cb_t)(char *buf, size_t buflen);

esp_err_t telegram_client_init(void);
esp_err_t telegram_send(const char *text);
esp_err_t telegram_send_alert(const char *message);
void telegram_register_status_cb(telegram_status_cb_t cb);
void telegram_register_telemetry_cb(telegram_telemetry_cb_t cb);
bool telegram_is_muted(void);
