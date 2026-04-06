/**
 * @file bist_logger.c
 * @brief BIST circular log on W25Q64 flash, one record per minute.
 */
#include "bist_logger.h"
#include "ext_flash.h"
#include "watchdog.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "bist_log";

#define BIST_MAGIC           0xBEEF1234U
#define BIST_RECORD_SIZE     256
#define BIST_FLASH_SIZE      (8UL * 1024 * 1024)
#define BIST_MAX_RECORDS     (BIST_FLASH_SIZE / BIST_RECORD_SIZE)   /* 32768 */
#define BIST_SECTOR_SIZE     4096
#define BIST_LOG_INTERVAL_S  60

/* The record written to flash */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq_num;
    uint32_t uptime_s;
    float    temp_c;
    float    humidity_pct;
    float    pressure_hpa;
    float    co_ppm;
    float    nh3_ppm;
    float    lux;
    uint8_t  gas_status;
    uint8_t  wifi_connected;
    uint8_t  alarm_active;
    uint8_t  _reserved;
    uint32_t heap_free_b;
    uint32_t psram_free_b;
    uint32_t cam_fps;
    uint32_t sensor_errors;
    uint8_t  padding[196];
    uint32_t crc32;
} bist_record_t;

_Static_assert(sizeof(bist_record_t) == BIST_RECORD_SIZE,
               "bist_record_t must be exactly 256 bytes");

static uint32_t s_write_idx  = 0;   /* Next record slot index (0..32767) */
static uint32_t s_boot_count = 0;   /* Records written this session */
static bool     s_ready      = false;

static uint32_t idx_to_addr(uint32_t idx)
{
    return (idx % BIST_MAX_RECORDS) * BIST_RECORD_SIZE;
}

esp_err_t bist_logger_init(void)
{
    /* Scan flash to find the last valid record (highest seq_num with good magic) */
    ESP_LOGI(TAG, "Scanning flash for latest BIST record...");

    uint32_t best_seq = 0;
    uint32_t best_idx = 0;
    bool     found    = false;

    /* Scan every 16th record for speed (256 × 16 = 4096 = one sector step) */
    for (uint32_t i = 0; i < BIST_MAX_RECORDS; i += 16) {
        uint32_t addr = idx_to_addr(i);
        uint32_t magic = 0;
        if (ext_flash_read(addr, &magic, 4) == ESP_OK && magic == BIST_MAGIC) {
            uint32_t seq = 0;
            ext_flash_read(addr + 4, &seq, 4);
            if (!found || seq > best_seq) {
                best_seq = seq;
                best_idx = i;
                found    = true;
            }
        }
    }

    if (found) {
        s_write_idx = (best_idx + 1) % BIST_MAX_RECORDS;
        ESP_LOGI(TAG, "Last record: idx=%lu seq=%lu. Next write at idx=%lu.",
                 (unsigned long)best_idx, (unsigned long)best_seq,
                 (unsigned long)s_write_idx);
    } else {
        s_write_idx = 0;
        ESP_LOGI(TAG, "No valid records found. Starting fresh at index 0.");
    }

    s_ready = true;
    return ESP_OK;
}

esp_err_t bist_logger_write(const telemetry_t *t)
{
    if (!s_ready || !t) return ESP_ERR_INVALID_STATE;

    uint32_t addr = idx_to_addr(s_write_idx);

    /* Erase sector when crossing a 4 KB boundary */
    if (addr % BIST_SECTOR_SIZE == 0) {
        ESP_LOGD(TAG, "Erasing sector at 0x%06lX.", (unsigned long)addr);
        esp_err_t ret = ext_flash_erase_sector(addr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Sector erase failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    bist_record_t rec = {0};
    rec.magic         = BIST_MAGIC;
    rec.seq_num       = s_write_idx;
    rec.uptime_s      = t->uptime_s;
    rec.temp_c        = t->temp_c;
    rec.humidity_pct  = t->humidity_pct;
    rec.pressure_hpa  = t->pressure_hpa;
    rec.co_ppm        = t->co_ppm;
    rec.nh3_ppm       = t->nh3_ppm;
    rec.lux           = t->lux;
    rec.gas_status    = (uint8_t)t->gas_status;
    rec.wifi_connected = t->wifi_connected ? 1 : 0;
    rec.alarm_active  = t->alarm_active ? 1 : 0;
    rec.heap_free_b   = t->heap_free_b;
    rec.psram_free_b  = t->psram_free_b;
    rec.cam_fps       = t->cam_fps_1s;
    rec.sensor_errors = t->sensor_error_count;

    /* CRC32 over all bytes except the last 4 */
    rec.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&rec,
                                  BIST_RECORD_SIZE - sizeof(uint32_t));

    esp_err_t ret = ext_flash_write_page(addr, &rec, BIST_RECORD_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write failed at 0x%06lX: %s",
                 (unsigned long)addr, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "BIST record written: idx=%lu addr=0x%06lX uptime=%lu s.",
             (unsigned long)s_write_idx, (unsigned long)addr,
             (unsigned long)t->uptime_s);

    s_write_idx = (s_write_idx + 1) % BIST_MAX_RECORDS;
    s_boot_count++;
    return ESP_OK;
}

static void bist_task(void *arg)
{
    watchdog_subscribe_current_task();

    ESP_LOGI(TAG, "BIST logger task started (interval: %d s).", BIST_LOG_INTERVAL_S);

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS((uint32_t)BIST_LOG_INTERVAL_S * 1000);

    while (true) {
        vTaskDelayUntil(&last_wake, period);

        telemetry_t snap;
        telemetry_snapshot(&snap);
        bist_logger_write(&snap);

        watchdog_reset();
    }
}

esp_err_t bist_logger_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        bist_task, "bist_log",
        4096, NULL,
        2,      /* Low priority — background logging */
        NULL,
        0       /* Core 0 */
    );
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

uint32_t bist_logger_get_count(void)
{
    return s_boot_count;
}
