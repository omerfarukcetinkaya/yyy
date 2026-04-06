/**
 * @file watchdog.h
 * @brief Task Watchdog Timer (TWDT) configuration and task registration.
 *
 * The TWDT is configured in sdkconfig (10s timeout, panic on trigger).
 * This module adds the main pipeline tasks to the watchdog and provides
 * a reset function they call in their main loop.
 */
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Configure the TWDT. Call once from app_main() after scheduler start.
 * sdkconfig already enables TWDT; this sets per-task subscriptions.
 */
esp_err_t watchdog_init(void);

/**
 * @brief Subscribe the calling task to the TWDT.
 * Call this from inside the task to be watched, after it has started.
 */
esp_err_t watchdog_subscribe_current_task(void);

/**
 * @brief Reset (pet) the watchdog for the calling task.
 * Call this regularly inside the task's main loop.
 */
void watchdog_reset(void);

/**
 * @brief Unsubscribe the calling task from the TWDT.
 * Call before a task exits cleanly.
 */
void watchdog_unsubscribe_current_task(void);
