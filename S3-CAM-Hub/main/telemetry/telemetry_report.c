/**
 * @file telemetry_report.c
 * @brief Central telemetry aggregation.
 */
#include "telemetry_report.h"
#include "system_monitor.h"
#include "fault_handler.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "telemetry";

static telemetry_t     s_telem;
static SemaphoreHandle_t s_mutex = NULL;

void telemetry_init(void)
{
    memset(&s_telem, 0, sizeof(s_telem));
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    strncpy(s_telem.reset_reason, fault_handler_reset_reason_str(),
            sizeof(s_telem.reset_reason) - 1);
    strncpy(s_telem.classifier_label, "none", sizeof(s_telem.classifier_label) - 1);
    strncpy(s_telem.alarm_last_reason, "none", sizeof(s_telem.alarm_last_reason) - 1);
    strncpy(s_telem.gas_status_str, "NORMAL", sizeof(s_telem.gas_status_str) - 1);
    ESP_LOGI(TAG, "Telemetry initialized.");
}

void telemetry_snapshot(telemetry_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_telem, sizeof(telemetry_t));
    xSemaphoreGive(s_mutex);
}

void telemetry_set_wifi(bool connected, const char *ip, int8_t rssi, uint32_t reconnects)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.wifi_connected     = connected;
    s_telem.wifi_rssi_dbm      = rssi;
    s_telem.wifi_reconnect_count = reconnects;
    if (ip) strncpy(s_telem.wifi_ip, ip, sizeof(s_telem.wifi_ip) - 1);
    xSemaphoreGive(s_mutex);
}

void telemetry_set_camera(uint32_t fps, uint32_t frames, uint32_t drops)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.cam_fps_1s     = fps;
    s_telem.cam_frame_count = frames;
    s_telem.cam_drop_count  = drops;
    xSemaphoreGive(s_mutex);
}

void telemetry_set_motion(uint32_t fps, bool detected, float score)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.motion_fps_1s   = fps;
    s_telem.motion_detected = detected;
    s_telem.motion_score    = score;
    xSemaphoreGive(s_mutex);
}

void telemetry_set_classifier(uint32_t fps, const char *label, float confidence)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.classifier_fps_1s    = fps;
    s_telem.classifier_confidence = confidence;
    if (label) strncpy(s_telem.classifier_label, label,
                       sizeof(s_telem.classifier_label) - 1);
    xSemaphoreGive(s_mutex);
}

void telemetry_set_sensors(float temp_c, float humidity, float pressure,
                           float co_ppm, float nh3_ppm, float lux,
                           uint8_t gas_status, const char *gas_status_str,
                           uint32_t reads, uint32_t errors)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.temp_c             = temp_c;
    s_telem.humidity_pct       = humidity;
    s_telem.pressure_hpa       = pressure;
    s_telem.co_ppm             = co_ppm;
    s_telem.nh3_ppm            = nh3_ppm;
    s_telem.lux                = lux;
    s_telem.gas_status         = gas_status;
    s_telem.sensor_read_count  = reads;
    s_telem.sensor_error_count = errors;
    if (gas_status_str) {
        strncpy(s_telem.gas_status_str, gas_status_str,
                sizeof(s_telem.gas_status_str) - 1);
    }
    xSemaphoreGive(s_mutex);
}

void telemetry_set_alarm(bool active, uint32_t count, const char *reason,
                         uint32_t tg_sent, uint32_t tg_fail)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.alarm_active        = active;
    s_telem.alarm_count         = count;
    s_telem.telegram_sent_count = tg_sent;
    s_telem.telegram_fail_count = tg_fail;
    if (reason) strncpy(s_telem.alarm_last_reason, reason,
                        sizeof(s_telem.alarm_last_reason) - 1);
    xSemaphoreGive(s_mutex);
}

void telemetry_set_stream_clients(uint32_t count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.stream_client_count = count;
    xSemaphoreGive(s_mutex);
}

void telemetry_refresh_system(void)
{
    sysmon_snapshot_t snap;
    sysmon_take_snapshot(&snap);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_telem.uptime_s              = snap.uptime_s;
    s_telem.cpu_core0_pct         = snap.cpu_core0_pct;
    s_telem.cpu_core1_pct         = snap.cpu_core1_pct;
    s_telem.heap_free_b           = snap.heap_free;
    s_telem.heap_free_min_b       = snap.heap_free_min;
    s_telem.heap_largest_block_b  = snap.heap_largest_block;
    s_telem.internal_free_b       = snap.internal_free;
    s_telem.psram_free_b          = snap.psram_free;
    s_telem.psram_free_min_b      = snap.psram_free_min;
    s_telem.psram_largest_block_b = snap.psram_largest_block;
    s_telem.last_update_us        = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}
