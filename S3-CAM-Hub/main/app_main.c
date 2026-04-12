/**
 * @file app_main.c
 * @brief yyy Vision Hub — entry point and subsystem orchestration.
 *
 * Boot sequence:
 *   1. NVS + event loop (prerequisites for Wi-Fi)
 *   2. Fault handler (reads reset reason before anything else might crash)
 *   3. Watchdog (configures TWDT)
 *   4. Telemetry init (all modules write into it, so init first)
 *   5. Web auth (precompute Base64 token once)
 *   6. Wi-Fi manager (starts STA; HTTP server auto-starts on IP event)
 *      — moved before camera so admin panel is up even if SCCB hangs
 *   7. Camera pipeline + driver (Core 1 DMA starts here)
 *   8. Alarm engine (Core 0)
 *   9. Vision pipeline tasks (Core 1: motion, classifier)
 *  10. Sensor hub (Core 0)
 *  11. Ext flash + BIST logger
 *  12. OLED display
 *  13. Telemetry task (Core 0, reports every 2 seconds)
 *
 * Core allocation:
 *   Core 0: Wi-Fi stack, HTTP server workers, sensor hub, alarm, telemetry
 *   Core 1: Camera DMA (esp_camera), motion detect, classifier
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

/* System */
#include "system/fault_handler.h"
#include "system/system_monitor.h"
#include "system/watchdog.h"

/* Camera */
#include "cam/cam_driver.h"
#include "cam/cam_pipeline.h"
#include "cam/frame_pool.h"

/* Vision */
#include "vision/motion_detect.h"
#include "vision/roi_tracker.h"
#include "vision/classifier.h"

/* Network */
#include "net/wifi_manager.h"
#include "net/web_auth.h"
#include "net/http_server.h"
#include "net/stream_handler.h"
#include "net/ws_stream.h"

/* Sensors */
#include "sensors/sensor_hub.h"

/* Alarm */
#include "alarm/alarm_engine.h"
#include "alarm/telegram_transport.h"

/* Display */
#include "display/oled_display.h"

/* Storage */
#include "storage/ext_flash.h"
#include "storage/bist_logger.h"

/* Telemetry */
#include "telemetry/telemetry_report.h"
#include "telemetry/uart_reporter.h"

static const char *TAG = "main";

/* ── Display task (1 Hz OLED refresh) ────────────────────────────────────── */

static void display_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000);

    while (true) {
        telemetry_t snap;
        telemetry_snapshot(&snap);
        oled_display_update(&snap);
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ── Telemetry task ───────────────────────────────────────────────────────── */

static void telemetry_task(void *arg)
{
    watchdog_subscribe_current_task();

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(2000);

    ESP_LOGI(TAG, "Telemetry task started (2s period).");

    while (true) {
        /* Refresh system-level metrics (heap, PSRAM, uptime) */
        telemetry_refresh_system();

        /* Pull camera pipeline stats */
        cam_pipeline_stats_t cam_stats;
        cam_pipeline_get_stats(&cam_stats);
        telemetry_set_camera(cam_stats.fps_1s,
                             cam_stats.frame_count,
                             cam_stats.drop_count);

        /* Pull Wi-Fi status */
        telemetry_set_wifi(wifi_manager_is_connected(),
                           wifi_manager_get_ip(),
                           wifi_manager_get_rssi(),
                           wifi_manager_get_reconnect_count());

        /* Pull stream client count (WebSocket broadcaster) */
        telemetry_set_stream_clients(ws_stream_get_client_count());

        /* Print 0.5 Hz UART report */
        telemetry_t snap;
        telemetry_snapshot(&snap);
        uart_reporter_print(&snap);

        watchdog_reset();
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ── app_main ─────────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 1. NVS ────────────────────────────────────────────────────────── */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (ret=%s). Erasing...",
                 esp_err_to_name(nvs_ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* ── 2. Default event loop (required before Wi-Fi) ─────────────────── */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ── 3. Fault handler ───────────────────────────────────────────────── */
    ESP_ERROR_CHECK(fault_handler_init());

    /* ── 4. Watchdog ────────────────────────────────────────────────────── */
    ESP_ERROR_CHECK(watchdog_init());

    /* ── 5. Telemetry (init before any module writes to it) ─────────────── */
    telemetry_init();

    /* ── 6. Web auth (precompute once) ──────────────────────────────────── */
    web_auth_init();

    /* ── 7. Wi-Fi (HTTP server starts automatically on IP event) ─────────
     * Start Wi-Fi BEFORE camera so the admin panel comes up regardless of
     * how long camera init takes (SCCB stuck after power cycle can add
     * 20-30 s to camera init; Wi-Fi runs in parallel on Core 0). */
    ESP_ERROR_CHECK(wifi_manager_init());

    /* ── 8. Camera pipeline state + frame pool ─────────────────────────── */
    ESP_ERROR_CHECK(cam_pipeline_init());
    ESP_ERROR_CHECK(frame_pool_init());

    /* ── 9. Camera hardware ─────────────────────────────────────────────── */
    esp_err_t cam_ret = cam_driver_init();
    if (cam_ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed (%s). Continuing without camera.",
                 esp_err_to_name(cam_ret));
        telemetry_increment_cam_init_error();
    } else {
        /* Start the dedicated cam producer task (Core 1, prio 6). */
        ESP_ERROR_CHECK(cam_pipeline_start());
    }

    /* ── 10. Alarm engine (Core 0) — init BEFORE vision tasks so mutex exists */
    const alert_transport_t *transport = telegram_transport_get();
    ESP_ERROR_CHECK(alarm_engine_init(transport));
    ESP_ERROR_CHECK(alarm_engine_start());

    /* ── 10. Vision pipeline tasks (Core 1) ─────────────────────────────── */
    ESP_ERROR_CHECK(motion_detect_init());
    ESP_ERROR_CHECK(motion_detect_start());

    ESP_ERROR_CHECK(roi_tracker_init());

    ESP_ERROR_CHECK(classifier_init());
    ESP_ERROR_CHECK(classifier_start());

    /* ── 11. Sensor hub (Core 0) ────────────────────────────────────────── */
    ESP_ERROR_CHECK(sensor_hub_init());
    ESP_ERROR_CHECK(sensor_hub_start());

    /* ── 12. External flash (W25Q64) + BIST logger ───────────────────────── */
    esp_err_t flash_ret = ext_flash_init();
    if (flash_ret == ESP_OK) {
        if (bist_logger_init() == ESP_OK) {
            ESP_ERROR_CHECK(bist_logger_start());
        } else {
            ESP_LOGW(TAG, "BIST logger init failed. Continuing without flash log.");
        }
    } else {
        ESP_LOGW(TAG, "W25Q64 not found (%s). BIST logging disabled.",
                 esp_err_to_name(flash_ret));
    }

    /* ── 13. OLED display ────────────────────────────────────────────────── */
    esp_err_t oled_ret = oled_display_init();
    if (oled_ret == ESP_OK) {
        BaseType_t disp_ret = xTaskCreatePinnedToCore(
            display_task, "display",
            3072, NULL,
            2,    /* Low priority — UI only */
            NULL,
            0     /* Core 0 */
        );
        if (disp_ret != pdPASS) {
            ESP_LOGW(TAG, "Display task create failed.");
        }
    } else {
        ESP_LOGW(TAG, "OLED init failed (%s). Display disabled.",
                 esp_err_to_name(oled_ret));
    }

    /* ── 15. Telemetry task (Core 0, lowest priority) ───────────────────── */
    BaseType_t telem_ret = xTaskCreatePinnedToCore(
        telemetry_task, "telemetry",
        6144,        /* Stack: enough for uart_reporter_print snprintf */
        NULL,
        2,           /* Priority 2 — lowest among application tasks */
        NULL,
        0            /* Core 0 */
    );
    configASSERT(telem_ret == pdPASS);

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  yyy Vision Hub — boot complete");
    ESP_LOGI(TAG, "  Board:   ESP32-S3-WROOM N16R8 + OV3660");
    ESP_LOGI(TAG, "  IDF:     %s", IDF_VER);
    ESP_LOGI(TAG, "  HTTP:    port %d (once Wi-Fi connects)", CONFIG_VH_WEB_PORT);
    ESP_LOGI(TAG, "  WS:      ws://<ip>:%d/ws/stream", CONFIG_VH_WEB_PORT);
    ESP_LOGI(TAG, "  Snapshot http://<ip>:%d/snapshot", CONFIG_VH_WEB_PORT);
    ESP_LOGI(TAG, "  Admin:   http://<ip>:%d/", CONFIG_VH_WEB_PORT);
    ESP_LOGI(TAG, "  Telem:   http://<ip>:%d/api/telemetry", CONFIG_VH_WEB_PORT);
    ESP_LOGI(TAG, "==============================================");

    /* app_main returns here — all work is in tasks. The FreeRTOS scheduler
       continues running all spawned tasks indefinitely. */
}
