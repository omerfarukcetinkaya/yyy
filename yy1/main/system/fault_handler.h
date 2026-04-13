/**
 * @file fault_handler.h
 * @brief Boot-time fault and reset-reason diagnostics.
 *
 * Call fault_handler_init() early in app_main() before any other subsystem.
 * Logs the reset reason to UART. On hard-fault or panic, the IDF default
 * handler runs; we extend it with a brief crash summary in NVS (future).
 */
#pragma once
#include "esp_err.h"

/**
 * @brief Initialize fault handler: read and log reset reason, clear fault NVS.
 * Must be called before any subsystem that might crash silently.
 */
esp_err_t fault_handler_init(void);

/**
 * @brief Get a short human-readable string for the last reset reason.
 * Returns a static string; do not free.
 */
const char *fault_handler_reset_reason_str(void);
