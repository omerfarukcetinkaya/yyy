#pragma once
#include "esp_err.h"

esp_err_t telegram_client_init(void);
esp_err_t telegram_send_alert(const char *message);
