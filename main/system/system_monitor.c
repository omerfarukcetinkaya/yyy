/**
 * @file system_monitor.c
 * @brief Runtime system statistics: heap, PSRAM, task HWMs.
 */
#include "system_monitor.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "sysmon";

/* Previous counters for delta CPU% calculation.
 * Task runtimes are in µs (ESP_TIMER mode). We use esp_timer_get_time()
 * as the elapsed-time base — total_runtime from uxTaskGetSystemState()
 * returns 0 in ESP-IDF v5 SMP mode and cannot be used. */
static int64_t  s_prev_time_us = 0;
static uint32_t s_prev_idle0   = 0;
static uint32_t s_prev_idle1   = 0;

void sysmon_take_snapshot(sysmon_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* ── Heap ───────────────────────────────────────────────────────────── */
    out->heap_free          = esp_get_free_heap_size();
    out->heap_free_min      = esp_get_minimum_free_heap_size();
    out->heap_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    /* ── Internal SRAM ──────────────────────────────────────────────────── */
    out->internal_free     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->internal_free_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    /* ── PSRAM ──────────────────────────────────────────────────────────── */
    out->psram_free          = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_free_min      = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    out->psram_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    /* ── Uptime ─────────────────────────────────────────────────────────── */
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    /* ── Task runtime stats + HWMs ──────────────────────────────────────── */
    TaskStatus_t task_buf[SYSMON_MAX_TASKS];
    uint32_t total_runtime = 0;
    UBaseType_t n = uxTaskGetSystemState(task_buf, SYSMON_MAX_TASKS, &total_runtime);
    out->task_count = (uint8_t)(n < SYSMON_MAX_TASKS ? n : SYSMON_MAX_TASKS);

    /* Look up idle tasks by handle — immune to name differences across IDF versions */
    TaskHandle_t idle0_h = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle1_h = xTaskGetIdleTaskHandleForCore(1);

    uint32_t idle0 = 0, idle1 = 0;
    for (uint8_t i = 0; i < out->task_count; i++) {
        strncpy(out->tasks[i].name, task_buf[i].pcTaskName,
                sizeof(out->tasks[i].name) - 1);
        out->tasks[i].hwm_words = task_buf[i].usStackHighWaterMark;

        if (task_buf[i].xHandle == idle0_h)
            idle0 = task_buf[i].ulRunTimeCounter;
        else if (task_buf[i].xHandle == idle1_h)
            idle1 = task_buf[i].ulRunTimeCounter;
    }

    /* On first call, log all task names so we can verify idle handle matching */
    static bool s_tasks_logged = false;
    if (!s_tasks_logged) {
        s_tasks_logged = true;
        ESP_LOGI(TAG, "Task list (%u tasks, idle0=%p idle1=%p):",
                 (unsigned)n, (void*)idle0_h, (void*)idle1_h);
        for (UBaseType_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  [%p] %s  runtime=%lu",
                     (void*)task_buf[i].xHandle,
                     task_buf[i].pcTaskName,
                     (unsigned long)task_buf[i].ulRunTimeCounter);
        }
    }

    /* ── CPU% (delta since last call) ───────────────────────────────────── */
    /* total_runtime from uxTaskGetSystemState() returns 0 in ESP-IDF v5 SMP.
     * Instead use esp_timer_get_time() (µs) as the elapsed-time base.
     * Task runtime counters are also in µs (ESP_TIMER stats mode).
     * Core N busy% = 100 - (delta_idle_us * 100 / elapsed_us)            */
    int64_t now_us    = esp_timer_get_time();
    int64_t elapsed   = now_us - s_prev_time_us;
    if (elapsed > 0) {
        uint32_t di0 = idle0 - s_prev_idle0;
        uint32_t di1 = idle1 - s_prev_idle1;
        int32_t  c0  = (int32_t)(100 - (int64_t)di0 * 100 / elapsed);
        int32_t  c1  = (int32_t)(100 - (int64_t)di1 * 100 / elapsed);
        out->cpu_core0_pct = (uint8_t)(c0 < 0 ? 0 : c0 > 100 ? 100 : c0);
        out->cpu_core1_pct = (uint8_t)(c1 < 0 ? 0 : c1 > 100 ? 100 : c1);
    }

    s_prev_time_us = now_us;
    s_prev_idle0   = idle0;
    s_prev_idle1   = idle1;
}

void sysmon_log_snapshot(const sysmon_snapshot_t *snap)
{
    if (!snap) return;

    ESP_LOGI(TAG, "--- System Monitor ---");
    ESP_LOGI(TAG, "Uptime:        %lu s", (unsigned long)snap->uptime_s);
    ESP_LOGI(TAG, "Heap free:     %lu B  (min ever: %lu B, largest: %lu B)",
             (unsigned long)snap->heap_free,
             (unsigned long)snap->heap_free_min,
             (unsigned long)snap->heap_largest_block);
    ESP_LOGI(TAG, "Internal RAM:  %lu B  (min ever: %lu B)",
             (unsigned long)snap->internal_free,
             (unsigned long)snap->internal_free_min);
    ESP_LOGI(TAG, "PSRAM free:    %lu B  (min ever: %lu B, largest: %lu B)",
             (unsigned long)snap->psram_free,
             (unsigned long)snap->psram_free_min,
             (unsigned long)snap->psram_largest_block);
    ESP_LOGI(TAG, "Tasks (%u):", snap->task_count);
    for (uint8_t i = 0; i < snap->task_count; i++) {
        ESP_LOGI(TAG, "  %-16s  HWM: %lu words",
                 snap->tasks[i].name,
                 (unsigned long)snap->tasks[i].hwm_words);
    }
}
