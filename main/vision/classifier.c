/**
 * @file classifier.c
 * @brief Classifier — Phase 1 STUB.
 * TODO Phase 5: Integrate TFLite Micro or ESP-DL model.
 */
#include "classifier.h"
#include "cam_config.h"
#include "cam_pipeline.h"
#include "telemetry_report.h"
#include "watchdog.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "classifier";
static classifier_result_t s_result;

static void classifier_task(void *arg)
{
    watchdog_subscribe_current_task();
    ESP_LOGW(TAG, "Classifier task running (STUB — no model loaded).");

    while (true) {
        /* TODO Phase 5: acquire frame, run inference */
        telemetry_set_classifier(0, "none", 0.0f);
        watchdog_reset();
        vTaskDelay(pdMS_TO_TICKS(CLASSIFIER_SAMPLE_PERIOD_MS));
    }
}

esp_err_t classifier_init(void)
{
    memset(&s_result, 0, sizeof(s_result));
    strncpy(s_result.top1.label, "none", CLASSIFIER_LABEL_LEN - 1);
    ESP_LOGW(TAG, "Classifier initialized (STUB).");
    return ESP_OK;
}

esp_err_t classifier_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        classifier_task, "classifier",
        6144, NULL,
        3,     /* Lowest vision priority */
        NULL,
        1      /* Core 1 */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void classifier_get_result(classifier_result_t *out)
{
    if (out) memcpy(out, &s_result, sizeof(*out));
}
