#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t rgb_led_init(void);
void rgb_led_set(uint8_t r, uint8_t g, uint8_t b);
void rgb_led_off(void);
void rgb_led_alarm(bool active);
