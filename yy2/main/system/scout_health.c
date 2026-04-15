/**
 * @file scout_health.c
 * @brief Scout self-diagnostics collector task.
 */
#include "scout_health.h"
#include "wifi_dual.h"
#include "telegram_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "scout_health";

static scout_health_t s_health;
static SemaphoreHandle_t s_mutex = NULL;
static int64_t s_last_run_us = 0;
static uint64_t s_prev_idle_time = 0;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "ext-pin";
    case ESP_RST_SW:        return "sw-restart";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "other-wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static uint32_t load_reboot_count(void)
{
    nvs_handle_t h;
    uint32_t cnt = 0;
    if (nvs_open("scout", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "reboots", &cnt);
        cnt++;
        nvs_set_u32(h, "reboots", cnt);
        nvs_commit(h);
        nvs_close(h);
    }
    return cnt;
}

static uint8_t estimate_cpu_pct(void)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* Use idle task runtime to compute aggregate CPU busy %.
     * C5 is single-core: idle task is "IDLE0". */
    TaskStatus_t *arr = NULL;
    uint32_t n = uxTaskGetNumberOfTasks();
    arr = (TaskStatus_t *)pvPortMalloc(n * sizeof(TaskStatus_t));
    if (!arr) return 0;
    uint32_t total_rt;
    uint32_t cnt = uxTaskGetSystemState(arr, n, &total_rt);
    uint64_t idle_rt = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (strncmp(arr[i].pcTaskName, "IDLE", 4) == 0) {
            idle_rt += arr[i].ulRunTimeCounter;
        }
    }
    vPortFree(arr);

    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_last_run_us;
    if (dt < 100000) return s_health.cpu_pct;  /* too soon, reuse */

    uint64_t idle_delta = idle_rt - s_prev_idle_time;
    /* esp_timer uses 1 MHz; over dt microseconds idle should use dt at idle */
    if (dt <= 0) return 0;
    uint32_t busy_pct = 100 - (uint32_t)((idle_delta * 100ULL) / (uint64_t)dt);
    if (busy_pct > 100) busy_pct = 100;

    s_prev_idle_time = idle_rt;
    s_last_run_us = now;
    return (uint8_t)busy_pct;
#else
    return 0;
#endif
}

static void health_task(void *arg)
{
    /* Initial sample */
    s_last_run_us = esp_timer_get_time();
    s_prev_idle_time = 0;

    while (true) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);

        s_health.uptime_s          = (uint32_t)(esp_timer_get_time() / 1000000);
        s_health.heap_free_b       = (uint32_t)esp_get_free_heap_size();
        s_health.heap_min_free_b   = (uint32_t)esp_get_minimum_free_heap_size();
        s_health.heap_largest_block_b = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        s_health.internal_free_b   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s_health.task_count        = uxTaskGetNumberOfTasks();
        s_health.cpu_pct           = estimate_cpu_pct();
        s_health.wifi_connected    = wifi_dual_is_connected();
        s_health.wifi_on_5g        = wifi_dual_is_on_5g();
        strncpy(s_health.wifi_ip, wifi_dual_get_ip(), sizeof(s_health.wifi_ip) - 1);
        s_health.wifi_rssi         = wifi_dual_get_rssi();
        s_health.tg_muted          = telegram_is_muted();

        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t scout_health_init(void)
{
    memset(&s_health, 0, sizeof(s_health));
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_health.reset_reason = reset_reason_str(esp_reset_reason());
    s_health.reboot_count = load_reboot_count();

    ESP_LOGI(TAG, "Scout health init — reset=%s reboot#%lu",
             s_health.reset_reason, (unsigned long)s_health.reboot_count);
    return ESP_OK;
}

esp_err_t scout_health_start(void)
{
    BaseType_t r = xTaskCreate(health_task, "scout_hlth", 4096, NULL, 2, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

void scout_health_snapshot(scout_health_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_health, sizeof(*out));
    xSemaphoreGive(s_mutex);
}

void scout_health_inc_tg_poll_ok(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_health.tg_poll_ok++;
    xSemaphoreGive(s_mutex);
}
void scout_health_inc_tg_poll_fail(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_health.tg_poll_fail++;
    xSemaphoreGive(s_mutex);
}
void scout_health_inc_tg_sent_ok(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_health.tg_sent_ok++;
    xSemaphoreGive(s_mutex);
}
void scout_health_inc_tg_sent_fail(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_health.tg_sent_fail++;
    xSemaphoreGive(s_mutex);
}

#define BOOT_NOTIFY_MIN_INTERVAL_S   600   /* 10 minutes between boot msgs */

bool scout_health_should_send_boot_notification(void)
{
    nvs_handle_t h;
    if (nvs_open("scout", NVS_READONLY, &h) != ESP_OK) return true;
    int64_t last = 0;
    size_t sz = sizeof(last);
    nvs_get_blob(h, "boot_ts", &last, &sz);
    nvs_close(h);
    /* last is unix epoch seconds; compare with time(NULL). If NTP hasn't
     * synced yet, time(NULL) returns a small value (< 2020), in which case
     * allow sending. Otherwise require min interval. */
    time_t now = time(NULL);
    if (now < 1600000000) return true;  /* before 2020 — time not synced */
    return (now - last) >= BOOT_NOTIFY_MIN_INTERVAL_S;
}

void scout_health_mark_boot_notification_sent(void)
{
    nvs_handle_t h;
    if (nvs_open("scout", NVS_READWRITE, &h) != ESP_OK) return;
    int64_t now = (int64_t)time(NULL);
    nvs_set_blob(h, "boot_ts", &now, sizeof(now));
    nvs_commit(h);
    nvs_close(h);
}
