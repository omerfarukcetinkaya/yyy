/**
 * @file cam_pipeline.c
 * @brief Camera frame buffer pipeline: acquisition statistics and consumer API.
 *
 * Consumers call cam_pipeline_get_frame(), use the frame, then call
 * cam_pipeline_release_frame(). The esp_camera driver manages all DMA
 * buffering internally via its frame buffer pool (PSRAM).
 *
 * FPS tracking uses a sliding 1-second window: a 64-bit timestamp ring
 * of the last N frames, counting how many fall within the last second.
 */
#include "cam_pipeline.h"
#include "cam_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "cam_pipe";

#define FPS_RING_SIZE  64u

/* After this many consecutive NULL returns from esp_camera_fb_get(), the
 * pipeline triggers a camera reinit (GDMA freeze recovery).
 * Each failed call blocks ~4 s internally, so 8 failures ≈ 32 s. */
#define CAM_CONSEC_FAIL_REINIT  8

typedef struct {
    uint32_t frame_count;
    uint32_t drop_count;
    /* FPS ring: timestamps (us) of the last FPS_RING_SIZE frames */
    int64_t  fps_ring[FPS_RING_SIZE];
    uint32_t fps_ring_head;
    uint32_t consec_fail;   /* consecutive esp_camera_fb_get() failures */
} pipeline_state_t;

static pipeline_state_t s_state;
static SemaphoreHandle_t s_stats_mutex = NULL;
static bool s_initialized = false;

esp_err_t cam_pipeline_init(void)
{
    if (s_initialized) return ESP_OK;

    memset(&s_state, 0, sizeof(s_state));

    s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) {
        ESP_LOGE(TAG, "Failed to create stats mutex.");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Pipeline initialized.");
    return ESP_OK;
}

esp_err_t cam_pipeline_get_frame(cam_frame_t *frame, uint32_t timeout_ms)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    if (!cam_driver_is_ready()) {
        ESP_LOGE(TAG, "Camera not ready.");
        return ESP_FAIL;
    }

    /* esp_camera_fb_get() calls cam_take() which has an internal 4-second
     * blocking timeout via xQueueReceive. The timeout_ms parameter here is
     * advisory only — the actual wait is capped by the driver's 4 s limit.
     * We call once and let the driver handle its own timeout. */
    (void)timeout_ms;
    int64_t t_get_start = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    int64_t t_get_ms = (esp_timer_get_time() - t_get_start) / 1000;

    if (!fb) {
        ESP_LOGW(TAG, "fb_get NULL after %lld ms", t_get_ms);
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        s_state.drop_count++;
        s_state.consec_fail++;
        uint32_t consec = s_state.consec_fail;
        xSemaphoreGive(s_stats_mutex);

        /* GDMA freeze recovery: reinit camera after repeated failures */
        if (consec >= CAM_CONSEC_FAIL_REINIT) {
            ESP_LOGW(TAG, "Camera frozen (%lu consecutive fails). Reinitializing ...",
                     (unsigned long)consec);
            cam_driver_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_err_t reinit = cam_driver_init();
            xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
            s_state.consec_fail = 0;
            xSemaphoreGive(s_stats_mutex);
            if (reinit != ESP_OK) {
                ESP_LOGE(TAG, "Camera reinit failed: %s", esp_err_to_name(reinit));
            } else {
                ESP_LOGI(TAG, "Camera reinitialized successfully.");
            }
        }
        return ESP_ERR_TIMEOUT;
    }

    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    s_state.consec_fail = 0;
    s_state.frame_count++;
    s_state.fps_ring[s_state.fps_ring_head % FPS_RING_SIZE] = now;
    s_state.fps_ring_head++;
    uint32_t seq = s_state.frame_count;
    xSemaphoreGive(s_stats_mutex);

    /* Log first 5 frames and every 50th: size, dimensions, acquisition time */
    if (seq <= 5 || seq % 50 == 0) {
        ESP_LOGI(TAG, "[#%lu] get=%lld ms  size=%zu B  %ux%u",
                 (unsigned long)seq, t_get_ms,
                 fb->len, fb->width, fb->height);
    }

    frame->fb    = fb;
    frame->seq   = seq;
    frame->ts_us = now;

    return ESP_OK;
}

void cam_pipeline_release_frame(cam_frame_t *frame)
{
    if (!frame || !frame->fb) return;
    esp_camera_fb_return(frame->fb);
    frame->fb = NULL;
}

void cam_pipeline_get_stats(cam_pipeline_stats_t *out)
{
    if (!out) return;

    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);

    out->frame_count = s_state.frame_count;
    out->drop_count  = s_state.drop_count;

    /* Count frames whose timestamp is within the last second */
    int64_t now = esp_timer_get_time();
    int64_t window_start = now - 1000000LL;
    uint32_t fps = 0;
    for (uint32_t i = 0; i < FPS_RING_SIZE; i++) {
        if (s_state.fps_ring[i] >= window_start) fps++;
    }
    out->fps_1s = fps;

    xSemaphoreGive(s_stats_mutex);
}
