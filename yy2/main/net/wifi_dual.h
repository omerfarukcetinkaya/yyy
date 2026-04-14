#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t wifi_dual_init(void);
esp_err_t wifi_dual_switch_to_5g(void);
esp_err_t wifi_dual_switch_to_24g(void);
void wifi_dual_release_lock(void);
bool wifi_dual_is_connected(void);
bool wifi_dual_is_on_5g(void);
bool wifi_dual_got_first_ip(void);
const char *wifi_dual_get_ip(void);
int8_t wifi_dual_get_rssi(void);
