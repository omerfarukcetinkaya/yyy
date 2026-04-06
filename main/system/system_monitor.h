/**
 * @file system_monitor.h
 * @brief Runtime system statistics: heap, PSRAM, task stack high-water marks.
 *
 * Provides a snapshot struct that can be populated on demand and fed into
 * the telemetry report. All reads are non-blocking.
 */
#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** Maximum number of tasks tracked for HWM and CPU reporting.
 *  WiFi/LWIP/httpd alone spawn ~8 tasks; keep headroom above worst case. */
#define SYSMON_MAX_TASKS  40

typedef struct {
    /* Heap */
    uint32_t heap_free;           /**< Current free heap (bytes) */
    uint32_t heap_free_min;       /**< Minimum free heap since boot */
    uint32_t heap_largest_block;  /**< Largest contiguous free block */

    /* Internal SRAM */
    uint32_t internal_free;       /**< Free internal SRAM (MALLOC_CAP_INTERNAL) */
    uint32_t internal_free_min;   /**< Minimum free internal SRAM since boot */

    /* PSRAM */
    uint32_t psram_free;          /**< Free PSRAM (MALLOC_CAP_SPIRAM) */
    uint32_t psram_free_min;      /**< Minimum free PSRAM since boot */
    uint32_t psram_largest_block; /**< Largest contiguous free PSRAM block */

    /* CPU usage (delta since last snapshot, 0-100 per core) */
    uint8_t  cpu_core0_pct;       /**< Core 0 busy percentage */
    uint8_t  cpu_core1_pct;       /**< Core 1 busy percentage */

    /* Task stack high-water marks */
    uint8_t  task_count;
    struct {
        char     name[16];
        uint32_t hwm_words;       /**< Stack remaining in FreeRTOS words */
    } tasks[SYSMON_MAX_TASKS];

    /* Uptime */
    uint32_t uptime_s;
} sysmon_snapshot_t;

/**
 * @brief Populate a sysmon_snapshot_t with current system state.
 * Safe to call from any task context.
 */
void sysmon_take_snapshot(sysmon_snapshot_t *out);

/**
 * @brief Log a snapshot to UART at INFO level.
 */
void sysmon_log_snapshot(const sysmon_snapshot_t *snap);
