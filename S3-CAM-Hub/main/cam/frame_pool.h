/**
 * @file frame_pool.h
 * @brief Multi-consumer, zero-copy camera frame buffer pool.
 *
 * The camera DMA runs in a dedicated producer task (see cam_pipeline.c).
 * That task drains frames from esp_camera and copies each JPEG into a
 * PSRAM-allocated slot here, then publishes the slot as the "latest"
 * frame. Any number of consumers (WebSocket stream clients, motion
 * detector, classifier, /snapshot handler) can then request the latest
 * frame without copying it; each holds a reference via an atomic
 * refcount, and releases it when done.
 *
 * Key properties:
 *   - Producer never blocks on consumers. If all slots are held, the
 *     producer drops the new frame (increments acquire_fails) and
 *     retries on the next DMA frame.
 *   - Consumers can block-wait for a frame strictly newer than a given
 *     sequence number, using a FreeRTOS task notification. The pool
 *     maintains a subscriber list and notifies each registered task
 *     after every publish.
 *   - Lifetime is owned by refcount: a slot becomes reusable when its
 *     refcount returns to zero. "Being the latest" counts as 1 reference
 *     (held by the producer on behalf of get_latest callers).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Slot sizing ─────────────────────────────────────────────────────────────
 * 4 slots × 160 KB = 640 KB PSRAM. One slot carried by producer as "latest",
 * up to 3 held simultaneously by consumers (2 WS clients + motion) without
 * the producer ever starving. */
#define FRAME_POOL_SLOT_COUNT   4u
#define FRAME_POOL_SLOT_CAP     (160u * 1024u)

/* Maximum number of task-notification subscribers (motion + two WS clients
 * + headroom). Non-subscriber consumers still work via get_latest(). */
#define FRAME_POOL_MAX_SUBSCRIBERS 6u

typedef struct frame_slot {
    uint8_t    *buf;        /**< PSRAM buffer, capacity FRAME_POOL_SLOT_CAP */
    size_t      len;        /**< Current JPEG length */
    uint32_t    width;
    uint32_t    height;
    uint32_t    seq;        /**< Monotonic global sequence number */
    int64_t     ts_us;      /**< Capture timestamp (esp_timer_get_time) */
    atomic_int  refcount;   /**< 0 = free, >0 = held (latest+consumers) */
} frame_slot_t;

typedef struct {
    uint32_t publish_count;
    uint32_t acquire_fails;    /**< Producer had no free slot — frame dropped */
    uint32_t subscribers;
    uint32_t slots_in_use;     /**< Snapshot: slots with refcount > 0 */
} frame_pool_stats_t;

/**
 * @brief Allocate slot buffers in PSRAM, init mutex and state.
 * Call once from app_main() before cam_pipeline_start().
 */
esp_err_t frame_pool_init(void);

/**
 * @brief Register the calling (or given) task as a publish subscriber.
 * After each publish, frame_pool notifies this task via xTaskNotifyGive.
 * Use ulTaskNotifyTake(pdTRUE, ...) in the consumer to wait efficiently.
 * Pass NULL to register the current task.
 */
esp_err_t frame_pool_subscribe(TaskHandle_t task);

/**
 * @brief Producer-side: obtain a free slot ready for writing.
 * Iterates the slot array for refcount == 0. Returns NULL if none.
 * On success, the returned slot has refcount == 0 — the producer owns it
 * exclusively and must call frame_pool_publish() when finished (or simply
 * drop the slot without publishing if it fails to fill it).
 */
frame_slot_t *frame_pool_acquire_writable(void);

/**
 * @brief Producer-side: publish a filled slot as the new latest.
 *  - Sets slot->seq to next_seq (assigned inside the mutex), ts_us to now.
 *  - Adds a "latest" ref to the new slot.
 *  - Drops the previous latest's "latest" ref (possibly freeing it).
 *  - Notifies all subscriber tasks.
 * The caller must have set slot->len, slot->width, slot->height before call.
 */
void frame_pool_publish(frame_slot_t *slot);

/**
 * @brief Consumer-side: get the latest published slot, adding a reference.
 * Returns NULL if no frame has been published yet. Caller MUST eventually
 * call frame_pool_release() to drop the reference.
 */
frame_slot_t *frame_pool_get_latest(void);

/**
 * @brief Consumer-side: wait until a frame strictly newer than last_seq is
 * available, or timeout_ms elapses. Returns the newer slot (refcount +1)
 * or NULL on timeout. Pass last_seq=0 to get the next frame produced.
 *
 * The caller must have previously called frame_pool_subscribe() for its
 * task, otherwise this function will still work but block the full timeout.
 */
frame_slot_t *frame_pool_wait_newer(uint32_t last_seq, uint32_t timeout_ms);

/**
 * @brief Consumer-side: drop a reference. When refcount reaches 0, the
 * slot becomes available for the producer to reuse.
 */
void frame_pool_release(frame_slot_t *slot);

/**
 * @brief Remove a task from the subscriber list.
 * Must be called before the subscribed task is deleted (vTaskDelete) —
 * otherwise xTaskNotifyGive will write to a freed TCB.
 * Pass NULL to unsubscribe the current task.
 */
void frame_pool_unsubscribe(TaskHandle_t task);

/**
 * @brief Debug / telemetry snapshot.
 */
void frame_pool_get_stats(frame_pool_stats_t *out);
