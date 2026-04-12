/**
 * @file frame_pool.c
 * @brief Implementation of the multi-consumer camera frame pool.
 */
#include "frame_pool.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "frame_pool";

static struct {
    frame_slot_t     slots[FRAME_POOL_SLOT_COUNT];
    frame_slot_t    *latest;          /* Most recently published slot (NULL if none) */
    uint32_t         next_seq;        /* Global monotonic sequence */
    SemaphoreHandle_t mutex;          /* Guards latest pointer + subscribers + next_seq */
    TaskHandle_t     subs[FRAME_POOL_MAX_SUBSCRIBERS];
    uint32_t         sub_count;
    uint32_t         publish_count;
    uint32_t         acquire_fails;
    bool             initialized;
} s_pool;

esp_err_t frame_pool_init(void)
{
    if (s_pool.initialized) return ESP_OK;
    memset(&s_pool, 0, sizeof(s_pool));

    s_pool.mutex = xSemaphoreCreateMutex();
    if (!s_pool.mutex) {
        ESP_LOGE(TAG, "Mutex create failed.");
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t i = 0; i < FRAME_POOL_SLOT_COUNT; i++) {
        frame_slot_t *s = &s_pool.slots[i];
        s->buf = (uint8_t *)heap_caps_malloc(FRAME_POOL_SLOT_CAP,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->buf) {
            ESP_LOGE(TAG, "Slot %lu PSRAM alloc (%u B) failed.",
                     (unsigned long)i, (unsigned)FRAME_POOL_SLOT_CAP);
            /* Free any earlier slots we managed to allocate */
            for (uint32_t j = 0; j < i; j++) {
                heap_caps_free(s_pool.slots[j].buf);
                s_pool.slots[j].buf = NULL;
            }
            vSemaphoreDelete(s_pool.mutex);
            s_pool.mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
        atomic_store(&s->refcount, 0);
    }

    s_pool.initialized = true;
    ESP_LOGI(TAG, "Initialized: %u slots × %u KB = %u KB PSRAM",
             (unsigned)FRAME_POOL_SLOT_COUNT,
             (unsigned)(FRAME_POOL_SLOT_CAP / 1024),
             (unsigned)(FRAME_POOL_SLOT_COUNT * FRAME_POOL_SLOT_CAP / 1024));
    return ESP_OK;
}

esp_err_t frame_pool_subscribe(TaskHandle_t task)
{
    if (!s_pool.initialized) return ESP_ERR_INVALID_STATE;
    if (!task) task = xTaskGetCurrentTaskHandle();

    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
    /* Already subscribed? */
    for (uint32_t i = 0; i < s_pool.sub_count; i++) {
        if (s_pool.subs[i] == task) {
            xSemaphoreGive(s_pool.mutex);
            return ESP_OK;
        }
    }
    if (s_pool.sub_count >= FRAME_POOL_MAX_SUBSCRIBERS) {
        xSemaphoreGive(s_pool.mutex);
        ESP_LOGW(TAG, "Subscriber table full (%u).",
                 (unsigned)FRAME_POOL_MAX_SUBSCRIBERS);
        return ESP_ERR_NO_MEM;
    }
    s_pool.subs[s_pool.sub_count++] = task;
    uint32_t n = s_pool.sub_count;
    xSemaphoreGive(s_pool.mutex);
    ESP_LOGI(TAG, "Subscriber added (%s) — total %lu",
             pcTaskGetName(task), (unsigned long)n);
    return ESP_OK;
}

frame_slot_t *frame_pool_acquire_writable(void)
{
    if (!s_pool.initialized) return NULL;

    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
    for (uint32_t i = 0; i < FRAME_POOL_SLOT_COUNT; i++) {
        frame_slot_t *s = &s_pool.slots[i];
        if (atomic_load(&s->refcount) == 0) {
            /* Mark exclusive ownership by producer (temporary ref) so that
             * no consumer picks it up mid-fill. Note: it is not yet the
             * latest, so get_latest won't see it; but we still bump the
             * ref to keep the invariant "refcount==0 means free". */
            atomic_store(&s->refcount, 1);
            xSemaphoreGive(s_pool.mutex);
            return s;
        }
    }
    s_pool.acquire_fails++;
    xSemaphoreGive(s_pool.mutex);
    return NULL;
}

void frame_pool_publish(frame_slot_t *slot)
{
    if (!slot || !s_pool.initialized) return;

    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);

    slot->seq   = ++s_pool.next_seq;
    slot->ts_us = esp_timer_get_time();

    /* Transfer the "latest" ref: the current refcount==1 (from acquire)
     * becomes the "latest" ref, so no additional bump is needed. We just
     * drop the old latest's "latest" ref. */
    frame_slot_t *old_latest = s_pool.latest;
    s_pool.latest = slot;
    s_pool.publish_count++;

    /* Snapshot subscribers under the lock so the notify loop can run
     * outside the critical section. */
    TaskHandle_t subs_copy[FRAME_POOL_MAX_SUBSCRIBERS];
    uint32_t n_subs = s_pool.sub_count;
    for (uint32_t i = 0; i < n_subs; i++) subs_copy[i] = s_pool.subs[i];

    xSemaphoreGive(s_pool.mutex);

    /* Drop the producer's "latest" ref on the previous slot. If it hits 0,
     * the slot is now free for future acquire_writable(). */
    if (old_latest) {
        (void)atomic_fetch_sub(&old_latest->refcount, 1);
    }

    /* Wake every subscriber. */
    for (uint32_t i = 0; i < n_subs; i++) {
        if (subs_copy[i]) xTaskNotifyGive(subs_copy[i]);
    }
}

frame_slot_t *frame_pool_get_latest(void)
{
    if (!s_pool.initialized) return NULL;

    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
    frame_slot_t *slot = s_pool.latest;
    if (slot) {
        atomic_fetch_add(&slot->refcount, 1);
    }
    xSemaphoreGive(s_pool.mutex);
    return slot;
}

frame_slot_t *frame_pool_wait_newer(uint32_t last_seq, uint32_t timeout_ms)
{
    if (!s_pool.initialized) return NULL;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
        frame_slot_t *latest = s_pool.latest;
        if (latest && latest->seq > last_seq) {
            atomic_fetch_add(&latest->refcount, 1);
            xSemaphoreGive(s_pool.mutex);
            return latest;
        }
        xSemaphoreGive(s_pool.mutex);

        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return NULL;
        TickType_t remaining = deadline - now;
        /* Wait for next publish notification. pdTRUE clears the count. */
        (void)ulTaskNotifyTake(pdTRUE, remaining);
    }
}

void frame_pool_release(frame_slot_t *slot)
{
    if (!slot || !s_pool.initialized) return;
    (void)atomic_fetch_sub(&slot->refcount, 1);
}

void frame_pool_get_stats(frame_pool_stats_t *out)
{
    if (!out || !s_pool.initialized) {
        if (out) memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
    out->publish_count  = s_pool.publish_count;
    out->acquire_fails  = s_pool.acquire_fails;
    out->subscribers    = s_pool.sub_count;
    uint32_t in_use = 0;
    for (uint32_t i = 0; i < FRAME_POOL_SLOT_COUNT; i++) {
        if (atomic_load(&s_pool.slots[i].refcount) > 0) in_use++;
    }
    out->slots_in_use = in_use;
    xSemaphoreGive(s_pool.mutex);
}
