/**
 * @file bist_logger.h
 * @brief BIST circular log on W25Q64 external flash.
 *
 * Writes a 256-byte timestamped record to W25Q64 every 60 seconds.
 * Records are stored in a circular buffer starting at address 0x000000.
 * Capacity: 8MB / 256B = 32,768 records = ~22.7 days of 1-record/min logging.
 *
 * Record format (256 bytes total, ends with CRC32):
 *   magic(4) | seq(4) | uptime_s(4) | temp(4) | hum(4) | pres(4) | co(4) |
 *   nh3(4) | lux(4) | gas_status(1) | wifi(1) | alarm(1) | pad(1) |
 *   heap(4) | psram(4) | cam_fps(4) | sensor_err(4) | padding(196) | crc32(4)
 */
#pragma once
#include "esp_err.h"
#include "telemetry_report.h"

/**
 * @brief Initialize the BIST logger. Reads the current write index from flash.
 * Call after ext_flash_init().
 */
esp_err_t bist_logger_init(void);

/**
 * @brief Write one BIST record using the current telemetry snapshot.
 * Erases the sector when the write pointer crosses a 4 KB sector boundary.
 * Called periodically (once per minute) from the bist_logger_task.
 */
esp_err_t bist_logger_write(const telemetry_t *t);

/**
 * @brief Start the periodic BIST logging task (Core 0, every 60 seconds).
 */
esp_err_t bist_logger_start(void);

/** @brief Return how many records have been written since boot. */
uint32_t bist_logger_get_count(void);
