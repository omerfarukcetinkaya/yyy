#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t espnow_bridge_init(void);
bool espnow_bridge_is_s3_online(void);
bool espnow_bridge_alarm_active(void);
const char *espnow_bridge_alarm_reason(void);
