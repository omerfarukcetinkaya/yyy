/**
 * @file alarm_engine.h
 * @brief Alarm evaluation engine with configurable thresholds and cooldown.
 *
 * Evaluates alarm conditions from sensor readings and vision pipeline.
 * When a condition triggers:
 *   1. Logs to UART immediately
 *   2. Sets alarm state in telemetry
 *   3. Dispatches via registered alert_transport_t (e.g. Telegram)
 *   4. Enters cooldown (no re-fire for VH_ALARM_COOLDOWN_S seconds)
 *
 * Alarm conditions evaluated:
 *   - Temperature > VH_ALARM_TEMP_HIGH_C
 *   - CO > VH_ALARM_CO_HIGH_PPM
 *   - [Future] Motion detected + high temp → possible fire scenario
 *   - [Future] Sensor read failures exceeding threshold → hardware fault
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "alert_transport.h"
#include "sensors/sensor_hub.h"

/**
 * @brief Initialize the alarm engine.
 * @param transport  Alert transport to use (may be NULL to disable notifications).
 */
esp_err_t alarm_engine_init(const alert_transport_t *transport);

/**
 * @brief Start the alarm evaluation task (Core 0).
 */
esp_err_t alarm_engine_start(void);

/**
 * @brief Register an alert transport after initialization.
 * Can be called at any time; takes effect on next evaluation cycle.
 */
void alarm_engine_set_transport(const alert_transport_t *transport);

/**
 * @brief Feed current sensor readings into the alarm engine.
 * Called by sensor_hub after each successful read.
 */
void alarm_engine_feed_sensors(const sensor_readings_t *readings);

/**
 * @brief Feed motion detection result into alarm engine.
 */
void alarm_engine_feed_motion(bool detected, float score);

/**
 * @brief Return true if any alarm condition is currently active.
 */
bool alarm_engine_is_active(void);
