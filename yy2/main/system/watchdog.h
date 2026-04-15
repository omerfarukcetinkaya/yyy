#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t watchdog_init(void);
void watchdog_reset(void);
esp_err_t watchdog_subscribe_current_task(void);

#ifdef __cplusplus
}
#endif
