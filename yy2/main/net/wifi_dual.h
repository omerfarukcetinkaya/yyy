#pragma once
#include "esp_err.h"

esp_err_t wifi_dual_init(void);
esp_err_t wifi_dual_switch_to_5g(void);
esp_err_t wifi_dual_switch_to_24g(void);
bool wifi_dual_is_connected(void);
bool wifi_dual_is_on_5g(void);
