/**
 * @file uart_reporter.c
 * @brief 0.5 Hz UART telemetry report — rich BIST-style health dump (every 2s).
 */
#include "uart_reporter.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "uart_rpt";

void uart_reporter_print(const telemetry_t *t)
{
    /* Print as structured log lines so IDF monitor can parse them */
    ESP_LOGI(TAG, "========== HEALTH REPORT ==========");
    ESP_LOGI(TAG, "[SYS]  uptime=%lu s  reset=%s",
             (unsigned long)t->uptime_s, t->reset_reason);

    /* Wi-Fi */
    if (t->wifi_connected) {
        ESP_LOGI(TAG, "[WIFI] connected  ip=%-15s  rssi=%d dBm  reconnects=%lu",
                 t->wifi_ip, t->wifi_rssi_dbm,
                 (unsigned long)t->wifi_reconnect_count);
    } else {
        ESP_LOGW(TAG, "[WIFI] DISCONNECTED  reconnects=%lu",
                 (unsigned long)t->wifi_reconnect_count);
    }

    /* Camera */
    ESP_LOGI(TAG, "[CAM]  fps=%lu  frames=%lu  drops=%lu",
             (unsigned long)t->cam_fps_1s,
             (unsigned long)t->cam_frame_count,
             (unsigned long)t->cam_drop_count);

    /* Vision */
    ESP_LOGI(TAG, "[VIS]  motion_fps=%lu  detected=%s  score=%.2f",
             (unsigned long)t->motion_fps_1s,
             t->motion_detected ? "YES" : "no",
             t->motion_score);
    ESP_LOGI(TAG, "[VIS]  classifier_fps=%lu  label=%s  conf=%.2f",
             (unsigned long)t->classifier_fps_1s,
             t->classifier_label,
             t->classifier_confidence);

    /* Stream */
    ESP_LOGI(TAG, "[STRM] clients=%lu",
             (unsigned long)t->stream_client_count);

    /* CPU */
    ESP_LOGI(TAG, "[CPU]  core0=%u%%  core1=%u%%",
             (unsigned)t->cpu_core0_pct, (unsigned)t->cpu_core1_pct);

    /* Sensors */
    ESP_LOGI(TAG, "[SENS] temp=%.1fC  hum=%.1f%%  pres=%.1fhPa  co=%.1fppm  nh3=%.1fppm  lux=%.0f",
             t->temp_c, t->humidity_pct, t->pressure_hpa,
             t->co_ppm, t->nh3_ppm, t->lux);
    ESP_LOGI(TAG, "[SENS] gas=%s  reads=%lu  errors=%lu",
             t->gas_status_str,
             (unsigned long)t->sensor_read_count,
             (unsigned long)t->sensor_error_count);

    /* Memory */
    ESP_LOGI(TAG, "[MEM]  heap_free=%lu B  heap_min=%lu B  heap_max_blk=%lu B",
             (unsigned long)t->heap_free_b,
             (unsigned long)t->heap_free_min_b,
             (unsigned long)t->heap_largest_block_b);
    ESP_LOGI(TAG, "[MEM]  psram_free=%lu B  psram_min=%lu B  psram_max_blk=%lu B",
             (unsigned long)t->psram_free_b,
             (unsigned long)t->psram_free_min_b,
             (unsigned long)t->psram_largest_block_b);
    ESP_LOGI(TAG, "[MEM]  internal_free=%lu B",
             (unsigned long)t->internal_free_b);

    /* Alarm */
    if (t->alarm_active) {
        ESP_LOGW(TAG, "[ALRM] !! ACTIVE !!  reason=%s  count=%lu  tg_sent=%lu  tg_fail=%lu",
                 t->alarm_last_reason, (unsigned long)t->alarm_count,
                 (unsigned long)t->telegram_sent_count,
                 (unsigned long)t->telegram_fail_count);
    } else {
        ESP_LOGI(TAG, "[ALRM] clear  count=%lu  tg_sent=%lu  tg_fail=%lu",
                 (unsigned long)t->alarm_count,
                 (unsigned long)t->telegram_sent_count,
                 (unsigned long)t->telegram_fail_count);
    }

    /* Error counters */
    ESP_LOGI(TAG, "[ERR]  cam_init=%lu  cam_fb_to=%lu  wifi=%lu  sensor=%lu",
             (unsigned long)t->err_cam_init,
             (unsigned long)t->err_cam_fb_timeout,
             (unsigned long)t->err_wifi_fail,
             (unsigned long)t->err_sensor_read);

    ESP_LOGI(TAG, "===================================");
}
