/**
 * @file cam_pipeline.c
 * @brief Dedicated camera producer task that drains esp_camera DMA into
 *        frame_pool. See cam_pipeline.h for the design rationale.
 *
 * The producer task:
 *   - Runs pinned to Core 1 at priority 6 (above motion/classifier).
 *   - Calls esp_camera_fb_get() — blocks up to ~4 s internally.
 *   - Acquires a writable frame_pool slot and memcpy's the JPEG (PSRAM→PSRAM,
 *     ~150 µs for a 45 KB VGA JPEG at 80 MHz octal SPIRAM).
 *   - Publishes the slot. If no slot is free (all held by consumers), drops
 *     the frame and bumps drop_count — the camera driver is released
 *     immediately and we move on to the next DMA frame.
 *   - On repeated NULL returns, reinitializes the camera driver (GDMA
 *     freeze recovery).
 *
 * FPS is measured with a 64-entry sliding-window timestamp ring.
 */
#include "cam_pipeline.h"
#include "cam_driver.h"
#include "frame_pool.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "cam_pipe";

#define FPS_RING_SIZE           64u
#define CAM_CONSEC_FAIL_REINIT  8u      /* ~32 s of NULLs before recovery */

#define CAM_TASK_STACK          4096
#define CAM_TASK_PRIO           6
#define CAM_TASK_CORE           1

typedef struct {
    uint32_t frame_count;
    uint32_t drop_count;
    int64_t  fps_ring[FPS_RING_SIZE];
    uint32_t fps_ring_head;
    uint32_t consec_fail;
} pipeline_state_t;

static pipeline_state_t s_state;
static SemaphoreHandle_t s_stats_mutex = NULL;
static TaskHandle_t      s_task = NULL;
static bool              s_initialized = false;

/* ── Internal helpers ───────────────────────────────────────────────────── */

static inline void stats_record_publish(int64_t ts_us)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    s_state.consec_fail = 0;
    s_state.frame_count++;
    s_state.fps_ring[s_state.fps_ring_head % FPS_RING_SIZE] = ts_us;
    s_state.fps_ring_head++;
    xSemaphoreGive(s_stats_mutex);
}

static inline uint32_t stats_record_fail(void)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    s_state.drop_count++;
    s_state.consec_fail++;
    uint32_t c = s_state.consec_fail;
    xSemaphoreGive(s_stats_mutex);
    return c;
}

static inline void stats_record_drop_no_slot(void)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    s_state.drop_count++;
    xSemaphoreGive(s_stats_mutex);
}

/* ── Producer task ─────────────────────────────────────────────────────── */

static void cam_task(void *arg)
{
    ESP_LOGI(TAG, "cam_task running on core %d, prio %u.",
             xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));

    while (true) {
        if (!cam_driver_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        int64_t t_get_ms = (esp_timer_get_time() - t0) / 1000;

        if (!fb) {
            uint32_t consec = stats_record_fail();
            ESP_LOGW(TAG, "fb_get NULL after %lld ms (consec=%lu)",
                     t_get_ms, (unsigned long)consec);
            if (consec >= CAM_CONSEC_FAIL_REINIT) {
                ESP_LOGW(TAG, "Camera frozen (%lu NULLs). Reinitializing ...",
                         (unsigned long)consec);
                cam_driver_deinit();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_err_t r = cam_driver_init();
                xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
                s_state.consec_fail = 0;
                xSemaphoreGive(s_stats_mutex);
                if (r != ESP_OK) {
                    ESP_LOGE(TAG, "Reinit failed: %s", esp_err_to_name(r));
                    vTaskDelay(pdMS_TO_TICKS(2000));
                } else {
                    ESP_LOGI(TAG, "Camera reinitialized.");
                }
            }
            continue;
        }

        /* Fast-path sanity check */
        if (fb->len == 0 || fb->len > FRAME_POOL_SLOT_CAP) {
            ESP_LOGW(TAG, "Unexpected fb->len=%zu (cap=%u), dropping",
                     fb->len, (unsigned)FRAME_POOL_SLOT_CAP);
            esp_camera_fb_return(fb);
            stats_record_drop_no_slot();
            continue;
        }

        frame_slot_t *slot = frame_pool_acquire_writable();
        if (!slot) {
            /* All slots held by consumers. Drop this frame — DMA will keep
             * producing. This is normal transient pressure, not an error. */
            esp_camera_fb_return(fb);
            stats_record_drop_no_slot();
            continue;
        }

        /* Copy JPEG from camera DMA buffer into pool slot (PSRAM→PSRAM). */
        memcpy(slot->buf, fb->buf, fb->len);
        slot->len    = fb->len;
        slot->width  = fb->width;
        slot->height = fb->height;

        /* Return camera buffer ASAP so DMA has a free slot. */
        esp_camera_fb_return(fb);

        /* Publish — assigns seq, ts_us, notifies subscribers, transfers
         * the "latest" ref from old slot to this slot. */
        frame_pool_publish(slot);
        stats_record_publish(slot->ts_us);

        /* Periodic log */
        uint32_t seq = slot->seq;
        if (seq <= 5 || seq % 100 == 0) {
            ESP_LOGI(TAG, "[#%lu] get=%lld ms  size=%zu B  %lux%lu",
                     (unsigned long)seq, t_get_ms,
                     slot->len,
                     (unsigned long)slot->width,
                     (unsigned long)slot->height);
        }
        /* No artificial delay — the sensor's frame period (~40 ms at 25 fps)
         * naturally paces fb_get(). GRAB_LATEST keeps DMA continuously
         * running and returns the freshest frame. */
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t cam_pipeline_init(void)
{
    if (s_initialized) return ESP_OK;
    memset(&s_state, 0, sizeof(s_state));
    s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) {
        ESP_LOGE(TAG, "Stats mutex create failed.");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Pipeline state initialized.");
    return ESP_OK;
}

esp_err_t cam_pipeline_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_task) return ESP_OK;

    BaseType_t ret = xTaskCreatePinnedToCore(
        cam_task, "cam_task",
        CAM_TASK_STACK, NULL,
        CAM_TASK_PRIO, &s_task,
        CAM_TASK_CORE);

    if (ret != pdPASS || !s_task) {
        ESP_LOGE(TAG, "cam_task create failed.");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "cam_task started (core %d, prio %d, stack %u)",
             CAM_TASK_CORE, CAM_TASK_PRIO, (unsigned)CAM_TASK_STACK);
    return ESP_OK;
}

void cam_pipeline_get_stats(cam_pipeline_stats_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);

    out->frame_count = s_state.frame_count;
    out->drop_count  = s_state.drop_count;

    int64_t now = esp_timer_get_time();
    int64_t window_start = now - 1000000LL;
    uint32_t fps = 0;
    for (uint32_t i = 0; i < FPS_RING_SIZE; i++) {
        if (s_state.fps_ring[i] >= window_start) fps++;
    }
    out->fps_1s = fps;

    xSemaphoreGive(s_stats_mutex);
}
