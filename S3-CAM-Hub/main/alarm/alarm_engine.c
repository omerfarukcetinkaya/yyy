/**
 * @file alarm_engine.c
 * @brief Alarm evaluation engine.
 */
#include "alarm_engine.h"
#include "gas_analyzer.h"
#include "telemetry_report.h"
#include "watchdog.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "alarm";

/* ── Alarm state ──────────────────────────────────────────────────────────── */
typedef struct {
    bool    active;
    char    reason[64];
    int64_t last_fire_us;    /* Timestamp of last alarm trigger (for cooldown) */
    uint32_t count;
    uint32_t tg_sent;
    uint32_t tg_fail;
} alarm_state_t;

/* ── Input snapshot (written by feed_* functions) ─────────────────────────── */
typedef struct {
    float        temp_c;
    float        co_ppm;
    float        nh3_ppm;
    float        lux;
    gas_status_t gas_status;
    bool         motion;
    float        motion_score;
} alarm_inputs_t;

static alarm_state_t   s_state;
static alarm_inputs_t  s_inputs;
static SemaphoreHandle_t s_mutex;
static const alert_transport_t *s_transport = NULL;
static bool s_initialized = false;

#define COOLDOWN_US  ((int64_t)CONFIG_VH_ALARM_COOLDOWN_S * 1000000LL)

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static bool cooldown_expired(void)
{
    if (s_state.last_fire_us == 0) return true;
    return (esp_timer_get_time() - s_state.last_fire_us) >= COOLDOWN_US;
}

static void fire_alarm(const char *reason, alert_severity_t sev)
{
    if (!cooldown_expired()) {
        ESP_LOGD(TAG, "Alarm suppressed by cooldown: %s", reason);
        return;
    }

    s_state.active = true;
    strncpy(s_state.reason, reason, sizeof(s_state.reason) - 1);
    s_state.last_fire_us = esp_timer_get_time();
    s_state.count++;

    ESP_LOGW(TAG, "!! ALARM: %s (count=%lu)", reason, (unsigned long)s_state.count);

    /* Dispatch via transport */
    if (s_transport && s_transport->send) {
        alert_msg_t msg = {
            .severity    = sev,
            .timestamp_us = s_state.last_fire_us,
        };
        strncpy(msg.title, "yyy Vision Hub Alarm", sizeof(msg.title) - 1);
        snprintf(msg.body, sizeof(msg.body), "%s", reason);

        if (s_transport->send(&msg) == ESP_OK) {
            s_state.tg_sent++;
        } else {
            s_state.tg_fail++;
            ESP_LOGW(TAG, "Alert transport failed for: %s", reason);
        }
    }

    telemetry_set_alarm(true, s_state.count, reason, s_state.tg_sent, s_state.tg_fail);
}

static void clear_alarm_if_safe(void)
{
    if (!s_state.active) return;

    /* Alarm clears only when all conditions are back to normal */
    bool temp_ok = s_inputs.temp_c < (float)CONFIG_VH_ALARM_TEMP_HIGH_C;
    bool co_ok   = s_inputs.co_ppm < (float)CONFIG_VH_ALARM_CO_HIGH_PPM;
    bool gas_ok  = (s_inputs.gas_status == GAS_STATUS_NORMAL ||
                    s_inputs.gas_status == GAS_STATUS_POOR_AIR);

    if (temp_ok && co_ok && gas_ok) {
        s_state.active = false;
        ESP_LOGI(TAG, "Alarm cleared — all conditions normal.");
        telemetry_set_alarm(false, s_state.count, s_state.reason,
                            s_state.tg_sent, s_state.tg_fail);
    }
}

static void evaluate(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    alarm_inputs_t in = s_inputs;
    xSemaphoreGive(s_mutex);

    char reason[64];

    /* Gas analyzer category — escalate severe categories to alarm */
    switch (in.gas_status) {
        case GAS_STATUS_FIRE:
            snprintf(reason, sizeof(reason),
                     "FIRE detected: CO=%.1f NH3=%.1f T=%.1f",
                     in.co_ppm, in.nh3_ppm, in.temp_c);
            fire_alarm(reason, ALERT_SEV_CRITICAL);
            break;
        case GAS_STATUS_TOXIC_GAS:
            snprintf(reason, sizeof(reason),
                     "TOXIC GAS: CO=%.1f NH3=%.1f",
                     in.co_ppm, in.nh3_ppm);
            fire_alarm(reason, ALERT_SEV_CRITICAL);
            break;
        case GAS_STATUS_CABLE_BURNING:
            snprintf(reason, sizeof(reason),
                     "Cable burning: NH3=%.1f CO=%.1f",
                     in.nh3_ppm, in.co_ppm);
            fire_alarm(reason, ALERT_SEV_WARNING);
            break;
        case GAS_STATUS_DANGEROUS:
            snprintf(reason, sizeof(reason),
                     "Dangerous gas: CO=%.1f NH3=%.1f",
                     in.co_ppm, in.nh3_ppm);
            fire_alarm(reason, ALERT_SEV_WARNING);
            break;
        default:
            break;
    }

    /* Threshold-based overrides (Kconfig values still apply) */
    if (in.temp_c >= (float)CONFIG_VH_ALARM_TEMP_HIGH_C) {
        snprintf(reason, sizeof(reason), "High temp: %.1fC (limit: %d)",
                 in.temp_c, CONFIG_VH_ALARM_TEMP_HIGH_C);
        fire_alarm(reason, ALERT_SEV_CRITICAL);
    }
    if (in.co_ppm >= (float)CONFIG_VH_ALARM_CO_HIGH_PPM) {
        snprintf(reason, sizeof(reason), "High CO: %.1f ppm (limit: %d)",
                 in.co_ppm, CONFIG_VH_ALARM_CO_HIGH_PPM);
        fire_alarm(reason, ALERT_SEV_CRITICAL);
    }

    clear_alarm_if_safe();
}

static void alarm_task(void *arg)
{
    watchdog_subscribe_current_task();

    ESP_LOGI(TAG, "Alarm task started. Temp limit: %d°C, CO limit: %d ppm, Cooldown: %d s.",
             CONFIG_VH_ALARM_TEMP_HIGH_C, CONFIG_VH_ALARM_CO_HIGH_PPM,
             CONFIG_VH_ALARM_COOLDOWN_S);

    while (true) {
        evaluate();
        watchdog_reset();
        vTaskDelay(pdMS_TO_TICKS(500));  /* Evaluate every 500 ms */
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t alarm_engine_init(const alert_transport_t *transport)
{
    if (s_initialized) return ESP_OK;

    memset(&s_state, 0, sizeof(s_state));
    memset(&s_inputs, 0, sizeof(s_inputs));
    strncpy(s_state.reason, "none", sizeof(s_state.reason) - 1);

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_transport = transport;

    if (transport && transport->init) {
        transport->init();
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Alarm engine initialized. Transport: %s",
             transport ? transport->name : "none");
    return ESP_OK;
}

esp_err_t alarm_engine_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        alarm_task, "alarm",
        4096, NULL,
        4,    /* Above sensors, below Wi-Fi */
        NULL,
        0     /* Core 0 */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void alarm_engine_set_transport(const alert_transport_t *transport)
{
    s_transport = transport;
    if (transport && transport->init) transport->init();
}

void alarm_engine_feed_sensors(const sensor_readings_t *readings)
{
    if (!s_initialized || !readings) return;
    /* Suppress alarm evaluation when sensors are not connected / read failing */
    if (!readings->valid) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_inputs.temp_c     = readings->temp_c;
    s_inputs.co_ppm     = readings->co_ppm;
    s_inputs.nh3_ppm    = readings->nh3_ppm;
    s_inputs.lux        = readings->lux;
    s_inputs.gas_status = readings->gas_status;
    xSemaphoreGive(s_mutex);
}

void alarm_engine_feed_motion(bool detected, float score)
{
    if (!s_initialized) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_inputs.motion       = detected;
    s_inputs.motion_score = score;
    xSemaphoreGive(s_mutex);
}

bool alarm_engine_is_active(void)
{
    return s_state.active;
}
