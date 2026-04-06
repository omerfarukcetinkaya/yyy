/**
 * @file motion_detect.c
 * @brief Motion detection — Phase 1 STUB.
 *
 * TODO (Phase 4):
 * 1. Acquire VGA JPEG frame from cam_pipeline
 * 2. Decode to YUV or RGB using a lightweight JPEG decoder
 *    (or use a secondary camera stream in GRAYSCALE mode)
 * 3. Downsample to ~80x60 grayscale
 * 4. Compute per-pixel luminance difference with previous frame
 * 5. Threshold sum → motion score; find bounding box of changed pixels
 * 6. Feed result to alarm_engine and telemetry
 */
#include "motion_detect.h"
#include "cam_config.h"
#include "cam_pipeline.h"
#include "alarm_engine.h"
#include "telemetry_report.h"
#include "watchdog.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "motion";

static motion_result_t  s_result;
static SemaphoreHandle_t s_mutex;
static bool s_initialized = false;

static void motion_detect_task(void *arg)
{
    watchdog_subscribe_current_task();

    ESP_LOGW(TAG, "Motion detection task running (STUB — no detection active).");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(MOTION_SAMPLE_PERIOD_MS);

    while (true) {
        /* TODO Phase 4: acquire frame, run differencing, update s_result */

        /* Stub: report no motion, update telemetry */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_result.detected = false;
        s_result.score    = 0.0f;
        xSemaphoreGive(s_mutex);

        telemetry_set_motion(0, false, 0.0f);
        alarm_engine_feed_motion(false, 0.0f);

        watchdog_reset();
        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t motion_detect_init(void)
{
    if (s_initialized) return ESP_OK;
    memset(&s_result, 0, sizeof(s_result));
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t motion_detect_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        motion_detect_task, "motion",
        8192, NULL,
        4,     /* Priority below cam acquisition */
        NULL,
        1      /* Core 1 — vision processing core */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void motion_detect_get_result(motion_result_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_result, sizeof(motion_result_t));
    xSemaphoreGive(s_mutex);
}
