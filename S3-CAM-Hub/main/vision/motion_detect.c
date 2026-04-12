/**
 * @file motion_detect.c
 * @brief Motion detection with morphological closing, blob merging and
 *        persistent multi-object tracking.
 *
 * Layered pipeline (see motion_detect.h for rationale):
 *   diff → dilate → CC labeling → blob merge → track update → publish
 *
 * Buffers (allocated once):
 *   rgb565       80×60×2 = 9.6 KB   DRAM (decoder output — hot)
 *   curr_gray    80×60   = 4.8 KB   DRAM
 *   prev_gray    80×60   = 4.8 KB   DRAM
 *   mask         80×60   = 4.8 KB   PSRAM (sequential access)
 *   dilate_tmp   80×60   = 4.8 KB   PSRAM (sequential access)
 *   labels       80×60×2 = 9.6 KB   PSRAM
 *   tracks[6]    ~320 B            BSS
 * Total ~38 KB.
 */
#include "motion_detect.h"
#include "blob_finder.h"
#include "frame_pool.h"
#include "cam_config.h"
#include "alarm_engine.h"
#include "telemetry_report.h"
#include "watchdog.h"
#include "img_converters.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "motion";

/* ── Tuning ──────────────────────────────────────────────────────────────
 * SCALE_8X (80×60) — decoder throughput ~50–70 ms, CPU headroom.
 * Fine enough to separate people and close-range hands.
 */
#define MOTION_MASK_W               80u
#define MOTION_MASK_H               60u
#define MOTION_MASK_PIX             (MOTION_MASK_W * MOTION_MASK_H)
#define MOTION_SCALE_SHIFT          3           /* 1<<3 = 8 */

#define MOTION_DIFF_THRESHOLD       22u         /* per-pixel absdiff gate */
#define MOTION_MIN_BLOB_AREA        8u          /* reject noise/single px */
#define MOTION_MERGE_MARGIN_PX      4           /* bbox gap for blob merge */

/* Tracker tuning — all in MASK coordinates unless noted. */
#define MOTION_TRACK_MATCH_DIST_PX  12          /* cx/cy euclidean */
#define MOTION_TRACK_CONFIRM_HITS   2           /* frames to become visible */
#define MOTION_TRACK_HOLD_MISSES    10          /* sticky hold after last match */
#define MOTION_TRACK_SMOOTH_ALPHA   0.45f       /* new-sample weight */

/* Motion task config */
#define MOTION_TASK_STACK           6144
#define MOTION_TASK_PRIO            4
#define MOTION_TASK_CORE            1
#define FRAME_STRIDE                6           /* ~28 fps cam → ~4.6 fps motion */

#define FPS_RING_SIZE               32u

/* ── Internal per-track state ─────────────────────────────────────────── */

typedef struct {
    bool     active;          /**< slot occupied */
    uint16_t id;              /**< stable id (1..65535, wraps) */
    float    cx, cy;          /**< smoothed center, mask coords */
    float    w, h;            /**< smoothed size, mask coords */
    float    score_smooth;    /**< smoothed intensity 0..1 */
    uint32_t hits;            /**< total frames this track matched */
    uint32_t misses;          /**< consecutive frames without match */
    uint32_t age_frames;      /**< frames since birth */
    bool     matched_now;     /**< matched a blob in the current frame */
} track_state_t;

/* ── State ────────────────────────────────────────────────────────────── */

static struct {
    uint8_t  *prev_gray;
    uint8_t  *curr_gray;
    uint8_t  *rgb565;
    uint8_t  *mask;
    uint8_t  *dilate_tmp;
    uint16_t *labels;
    bool      have_prev;
    motion_result_t result;
    SemaphoreHandle_t mutex;
    int64_t  fps_ring[FPS_RING_SIZE];
    uint32_t fps_ring_head;
    track_state_t tracks[MOTION_MAX_TRACKS];
    uint16_t next_track_id;
} s_mo;

static bool s_initialized = false;

/* ── Image helpers ─────────────────────────────────────────────────────── */

static inline void rgb565_to_gray(const uint8_t *rgb565, uint8_t *gray, uint32_t n_pixels)
{
    /* esp_jpeg_decode with swap_color_bytes=0 writes [LO, HI] where
     * color = (R&0xF8)<<8 | (G&0xFC)<<3 | (B>>3).
     *   LO = [G4 G3 G2 | B7 B6 B5 B4 B3]
     *   HI = [R7 R6 R5 R4 R3 | G7 G6 G5]
     * Y = (77*R + 150*G + 29*B) >> 8. */
    for (uint32_t i = 0; i < n_pixels; i++) {
        uint8_t lo = rgb565[i * 2 + 0];
        uint8_t hi = rgb565[i * 2 + 1];
        uint32_t r  = (hi & 0xF8);
        uint32_t g  = ((hi & 0x07) << 5) | ((lo & 0xE0) >> 3);
        uint32_t bl = (lo & 0x1F) << 3;
        gray[i] = (uint8_t)((77u * r + 150u * g + 29u * bl) >> 8);
    }
}

static inline uint32_t build_diff_mask(const uint8_t *a, const uint8_t *b,
                                       uint8_t *out, uint32_t n, uint8_t thresh)
{
    uint32_t fg = 0;
    for (uint32_t i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) d = -d;
        if ((uint32_t)d > thresh) {
            out[i] = (uint8_t)d;
            fg++;
        } else {
            out[i] = 0;
        }
    }
    return fg;
}

/* 3×3 cross dilate: each output pixel = max of itself and N/S/E/W
 * neighbors. Preserves intensity (absdiff magnitude) while closing
 * 1-pixel gaps, so a fragmented head (eyes/ears/hair moving slightly
 * differently) merges into one solid region. */
static void dilate_cross_max(const uint8_t *in, uint8_t *out, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *row   = &in[y * w];
        const uint8_t *row_u = (y > 0)       ? &in[(y - 1) * w] : NULL;
        const uint8_t *row_d = (y + 1 < h)   ? &in[(y + 1) * w] : NULL;
        uint8_t       *orow  = &out[y * w];
        for (int x = 0; x < w; x++) {
            uint8_t v = row[x];
            if (x > 0)           { uint8_t t = row[x - 1];   if (t > v) v = t; }
            if (x + 1 < w)       { uint8_t t = row[x + 1];   if (t > v) v = t; }
            if (row_u)           { uint8_t t = row_u[x];     if (t > v) v = t; }
            if (row_d)           { uint8_t t = row_d[x];     if (t > v) v = t; }
            orow[x] = v;
        }
    }
}

/* ── Blob merging ──────────────────────────────────────────────────────── */

static inline int bbox_gap(int a_x1, int a_y1, int a_x2, int a_y2,
                           int b_x1, int b_y1, int b_x2, int b_y2,
                           int *out_gx, int *out_gy)
{
    int gx = 0, gy = 0;
    if (a_x1 > b_x2)      gx = a_x1 - b_x2;
    else if (b_x1 > a_x2) gx = b_x1 - a_x2;
    if (a_y1 > b_y2)      gy = a_y1 - b_y2;
    else if (b_y1 > a_y2) gy = b_y1 - a_y2;
    if (out_gx) *out_gx = gx;
    if (out_gy) *out_gy = gy;
    return (gx > gy) ? gx : gy;
}

static void merge_close_blobs(blob_result_t *br, int margin)
{
    bool merged_any;
    do {
        merged_any = false;
        for (uint32_t i = 0; i + 1 < br->count; i++) {
            for (uint32_t j = i + 1; j < br->count; ) {
                int a_x1 = br->blobs[i].x;
                int a_y1 = br->blobs[i].y;
                int a_x2 = a_x1 + br->blobs[i].w - 1;
                int a_y2 = a_y1 + br->blobs[i].h - 1;
                int b_x1 = br->blobs[j].x;
                int b_y1 = br->blobs[j].y;
                int b_x2 = b_x1 + br->blobs[j].w - 1;
                int b_y2 = b_y1 + br->blobs[j].h - 1;
                int gx, gy;
                int gap = bbox_gap(a_x1, a_y1, a_x2, a_y2,
                                   b_x1, b_y1, b_x2, b_y2, &gx, &gy);
                (void)gap;
                if (gx <= margin && gy <= margin) {
                    /* Union the two blobs into i, remove j. */
                    int u_x1 = (a_x1 < b_x1) ? a_x1 : b_x1;
                    int u_y1 = (a_y1 < b_y1) ? a_y1 : b_y1;
                    int u_x2 = (a_x2 > b_x2) ? a_x2 : b_x2;
                    int u_y2 = (a_y2 > b_y2) ? a_y2 : b_y2;
                    br->blobs[i].x = (uint16_t)u_x1;
                    br->blobs[i].y = (uint16_t)u_y1;
                    br->blobs[i].w = (uint16_t)(u_x2 - u_x1 + 1);
                    br->blobs[i].h = (uint16_t)(u_y2 - u_y1 + 1);
                    br->blobs[i].area          += br->blobs[j].area;
                    br->blobs[i].intensity_sum += br->blobs[j].intensity_sum;
                    br->blobs[i].score = (br->blobs[i].area > 0)
                        ? (float)br->blobs[i].intensity_sum / ((float)br->blobs[i].area * 255.0f)
                        : 0.0f;
                    /* Swap j with last and shrink. */
                    br->blobs[j] = br->blobs[br->count - 1];
                    br->count--;
                    merged_any = true;
                    /* re-check same j since it's now a different blob */
                } else {
                    j++;
                }
            }
        }
    } while (merged_any);
}

/* ── Tracker ──────────────────────────────────────────────────────────── */

static inline uint16_t alloc_track_id(void)
{
    uint16_t id = s_mo.next_track_id++;
    if (s_mo.next_track_id == 0) s_mo.next_track_id = 1;
    return id;
}

static void tracker_reset_match_flags(void)
{
    for (uint32_t t = 0; t < MOTION_MAX_TRACKS; t++) {
        s_mo.tracks[t].matched_now = false;
    }
}

/* Match each blob to the best unmatched active track (greedy, nearest
 * center within MATCH_DIST_PX). Blobs that don't match spawn new tracks
 * in free slots. */
static void tracker_update(const blob_result_t *br)
{
    const float MATCH_D2 = (float)(MOTION_TRACK_MATCH_DIST_PX *
                                   MOTION_TRACK_MATCH_DIST_PX);
    const float alpha = MOTION_TRACK_SMOOTH_ALPHA;

    for (uint32_t b = 0; b < br->count; b++) {
        float bcx = br->blobs[b].x + br->blobs[b].w * 0.5f;
        float bcy = br->blobs[b].y + br->blobs[b].h * 0.5f;

        int  best     = -1;
        float best_d2 = 1e9f;
        for (uint32_t t = 0; t < MOTION_MAX_TRACKS; t++) {
            if (!s_mo.tracks[t].active || s_mo.tracks[t].matched_now) continue;
            float dx = s_mo.tracks[t].cx - bcx;
            float dy = s_mo.tracks[t].cy - bcy;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = (int)t;
            }
        }

        if (best >= 0 && best_d2 <= MATCH_D2) {
            track_state_t *tr = &s_mo.tracks[best];
            tr->cx           = alpha * bcx + (1.0f - alpha) * tr->cx;
            tr->cy           = alpha * bcy + (1.0f - alpha) * tr->cy;
            tr->w            = alpha * (float)br->blobs[b].w + (1.0f - alpha) * tr->w;
            tr->h            = alpha * (float)br->blobs[b].h + (1.0f - alpha) * tr->h;
            tr->score_smooth = alpha * br->blobs[b].score    + (1.0f - alpha) * tr->score_smooth;
            tr->hits++;
            tr->misses = 0;
            tr->age_frames++;
            tr->matched_now = true;
            continue;
        }

        /* No matching track — try to spawn a new one. */
        int free_slot = -1;
        for (uint32_t t = 0; t < MOTION_MAX_TRACKS; t++) {
            if (!s_mo.tracks[t].active) { free_slot = (int)t; break; }
        }
        if (free_slot < 0) continue;   /* out of slots, drop the blob */

        track_state_t *tr = &s_mo.tracks[free_slot];
        tr->active       = true;
        tr->id           = alloc_track_id();
        tr->cx           = bcx;
        tr->cy           = bcy;
        tr->w            = (float)br->blobs[b].w;
        tr->h            = (float)br->blobs[b].h;
        tr->score_smooth = br->blobs[b].score;
        tr->hits         = 1;
        tr->misses       = 0;
        tr->age_frames   = 1;
        tr->matched_now  = true;
    }

    /* Advance unmatched active tracks: increment miss, retire if stale. */
    for (uint32_t t = 0; t < MOTION_MAX_TRACKS; t++) {
        track_state_t *tr = &s_mo.tracks[t];
        if (!tr->active || tr->matched_now) continue;
        tr->misses++;
        tr->age_frames++;
        if (tr->misses > MOTION_TRACK_HOLD_MISSES) {
            tr->active = false;
        }
    }
}

/* Fill motion_result_t from current tracker state. */
static void tracker_publish(motion_result_t *out)
{
    uint32_t visible = 0;
    float    max_score = 0.0f;

    for (uint32_t t = 0; t < MOTION_MAX_TRACKS; t++) {
        const track_state_t *tr = &s_mo.tracks[t];
        if (!tr->active) continue;
        if (tr->hits < MOTION_TRACK_CONFIRM_HITS) continue;
        if (visible >= MOTION_MAX_TRACKS) break;

        motion_track_out_t *o = &out->tracks[visible];

        int hw = (int)(tr->w * 0.5f + 0.5f);
        int hh = (int)(tr->h * 0.5f + 0.5f);
        int x1 = (int)(tr->cx + 0.5f) - hw;
        int y1 = (int)(tr->cy + 0.5f) - hh;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x1 >= (int)MOTION_MASK_W) x1 = MOTION_MASK_W - 1;
        if (y1 >= (int)MOTION_MASK_H) y1 = MOTION_MASK_H - 1;
        int w_m = (int)(tr->w + 0.5f);
        int h_m = (int)(tr->h + 0.5f);
        if (x1 + w_m > (int)MOTION_MASK_W) w_m = MOTION_MASK_W - x1;
        if (y1 + h_m > (int)MOTION_MASK_H) h_m = MOTION_MASK_H - y1;
        if (w_m < 1) w_m = 1;
        if (h_m < 1) h_m = 1;

        o->id         = tr->id;
        o->x          = (uint16_t)(x1 << MOTION_SCALE_SHIFT);
        o->y          = (uint16_t)(y1 << MOTION_SCALE_SHIFT);
        o->w          = (uint16_t)(w_m << MOTION_SCALE_SHIFT);
        o->h          = (uint16_t)(h_m << MOTION_SCALE_SHIFT);
        o->score      = tr->score_smooth;
        o->age_frames = tr->age_frames;
        o->hit_count  = tr->hits;
        o->lost       = (tr->misses > 0);
        if (tr->score_smooth > max_score) max_score = tr->score_smooth;
        visible++;
    }

    out->track_count   = visible;
    out->detected      = (visible > 0);
    out->global_score  = max_score;
}

/* ── FPS tracking ──────────────────────────────────────────────────────── */

static uint32_t fps_compute_1s(void)
{
    int64_t now = esp_timer_get_time();
    int64_t window = now - 1000000LL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < FPS_RING_SIZE; i++) {
        if (s_mo.fps_ring[i] >= window) n++;
    }
    return n;
}

static void fps_record(void)
{
    s_mo.fps_ring[s_mo.fps_ring_head % FPS_RING_SIZE] = esp_timer_get_time();
    s_mo.fps_ring_head++;
}

/* ── Motion task ──────────────────────────────────────────────────────── */

static void motion_task(void *arg)
{
    watchdog_subscribe_current_task();
    frame_pool_subscribe(NULL);

    ESP_LOGI(TAG, "motion_task running on core %d, prio %u",
             xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));

    uint32_t last_seq = 0;

    while (true) {
        uint32_t target = last_seq + FRAME_STRIDE;
        frame_slot_t *slot = frame_pool_wait_newer(target - 1, 2000);
        if (!slot) {
            watchdog_reset();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last_seq = slot->seq;

        int64_t t0 = esp_timer_get_time();

        bool ok = jpg2rgb565(slot->buf, slot->len, s_mo.rgb565, JPG_SCALE_8X);
        uint16_t src_w = (uint16_t)slot->width;
        uint16_t src_h = (uint16_t)slot->height;
        int64_t  ts_us = slot->ts_us;
        frame_pool_release(slot);

        if (!ok) {
            ESP_LOGW(TAG, "jpg2rgb565 failed (seq=%lu)", (unsigned long)last_seq);
            watchdog_reset();
            continue;
        }

        int64_t t_decode = esp_timer_get_time();

        rgb565_to_gray(s_mo.rgb565, s_mo.curr_gray, MOTION_MASK_PIX);

        uint32_t fg_px = 0;
        blob_result_t blobs = {0};

        if (s_mo.have_prev) {
            fg_px = build_diff_mask(s_mo.curr_gray, s_mo.prev_gray,
                                    s_mo.mask, MOTION_MASK_PIX,
                                    MOTION_DIFF_THRESHOLD);

            if (fg_px > 0) {
                /* Close 1-pixel gaps so fragmented body parts coalesce. */
                dilate_cross_max(s_mo.mask, s_mo.dilate_tmp,
                                 MOTION_MASK_W, MOTION_MASK_H);
                blob_finder_run(s_mo.dilate_tmp,
                                MOTION_MASK_W, MOTION_MASK_H,
                                MOTION_MIN_BLOB_AREA, s_mo.labels, &blobs);
                /* Merge blobs whose bboxes are close (iterative). */
                merge_close_blobs(&blobs, MOTION_MERGE_MARGIN_PX);
            }
        }
        memcpy(s_mo.prev_gray, s_mo.curr_gray, MOTION_MASK_PIX);
        s_mo.have_prev = true;

        /* ── Tracker update ────────────────────────────────────────── */
        tracker_reset_match_flags();
        if (blobs.count > 0) {
            tracker_update(&blobs);
        } else {
            /* No raw blobs this frame — just age existing tracks. */
            for (uint32_t t = 0; t < MOTION_MAX_TRACKS; t++) {
                track_state_t *tr = &s_mo.tracks[t];
                if (!tr->active) continue;
                tr->misses++;
                tr->age_frames++;
                if (tr->misses > MOTION_TRACK_HOLD_MISSES) {
                    tr->active = false;
                }
            }
        }

        int64_t t_done = esp_timer_get_time();

        /* ── Publish ───────────────────────────────────────────────── */
        fps_record();
        uint32_t fps1 = fps_compute_1s();

        xSemaphoreTake(s_mo.mutex, portMAX_DELAY);
        s_mo.result.seq        = last_seq;
        s_mo.result.ts_us      = ts_us;
        s_mo.result.src_width  = src_w;
        s_mo.result.src_height = src_h;
        s_mo.result.fps_1s     = fps1;
        tracker_publish(&s_mo.result);
        bool     any_det = s_mo.result.detected;
        float    g_score = s_mo.result.global_score;
        uint32_t n_tracks = s_mo.result.track_count;
        xSemaphoreGive(s_mo.mutex);

        telemetry_set_motion(fps1, any_det, g_score);
        alarm_engine_feed_motion(any_det, g_score);

        if (last_seq <= 3 || (last_seq % 40 == 0) || any_det) {
            ESP_LOGI(TAG,
                "[#%lu] dec=%lld ms proc=%lld ms fg=%lu raw_blobs=%lu tracks=%lu score=%.3f %s",
                (unsigned long)last_seq,
                (t_decode - t0) / 1000,
                (t_done - t_decode) / 1000,
                (unsigned long)fg_px,
                (unsigned long)blobs.count,
                (unsigned long)n_tracks,
                g_score,
                any_det ? "[MOTION]" : "");
        }

        watchdog_reset();
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t motion_detect_init(void)
{
    if (s_initialized) return ESP_OK;

    memset(&s_mo, 0, sizeof(s_mo));
    s_mo.next_track_id = 1;

    /* Hot buffers: DRAM for decoder output + gray planes. */
    s_mo.rgb565     = (uint8_t *)heap_caps_malloc(MOTION_MASK_PIX * 2,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_mo.curr_gray  = (uint8_t *)heap_caps_malloc(MOTION_MASK_PIX,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_mo.prev_gray  = (uint8_t *)heap_caps_malloc(MOTION_MASK_PIX,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /* Mask, dilate scratch, labels: PSRAM — accessed sequentially. */
    s_mo.mask       = (uint8_t *)heap_caps_malloc(MOTION_MASK_PIX, MALLOC_CAP_SPIRAM);
    s_mo.dilate_tmp = (uint8_t *)heap_caps_malloc(MOTION_MASK_PIX, MALLOC_CAP_SPIRAM);
    s_mo.labels     = (uint16_t *)heap_caps_malloc(MOTION_MASK_PIX * sizeof(uint16_t),
                                                   MALLOC_CAP_SPIRAM);

    if (!s_mo.rgb565 || !s_mo.curr_gray || !s_mo.prev_gray ||
        !s_mo.mask || !s_mo.dilate_tmp || !s_mo.labels) {
        ESP_LOGE(TAG, "motion buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_mo.mutex = xSemaphoreCreateMutex();
    if (!s_mo.mutex) return ESP_ERR_NO_MEM;

    s_mo.have_prev = false;
    s_initialized = true;
    ESP_LOGI(TAG, "Motion init OK  mask=%ux%u  tracks_max=%u",
             (unsigned)MOTION_MASK_W, (unsigned)MOTION_MASK_H,
             (unsigned)MOTION_MAX_TRACKS);
    return ESP_OK;
}

esp_err_t motion_detect_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    BaseType_t r = xTaskCreatePinnedToCore(
        motion_task, "motion",
        MOTION_TASK_STACK, NULL,
        MOTION_TASK_PRIO, NULL,
        MOTION_TASK_CORE);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}

void motion_detect_get_result(motion_result_t *out)
{
    if (!out || !s_initialized) { if (out) memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_mo.mutex, portMAX_DELAY);
    memcpy(out, &s_mo.result, sizeof(motion_result_t));
    xSemaphoreGive(s_mo.mutex);
}

size_t motion_detect_build_json(char *buf, size_t buflen)
{
    if (!buf || buflen < 64 || !s_initialized) return 0;

    xSemaphoreTake(s_mo.mutex, portMAX_DELAY);
    motion_result_t r = s_mo.result;
    xSemaphoreGive(s_mo.mutex);

    int n = snprintf(buf, buflen,
        "{\"t\":\"motion\",\"seq\":%lu,\"det\":%s,\"score\":%.3f,"
        "\"fps\":%lu,\"w\":%u,\"h\":%u,\"tracks\":[",
        (unsigned long)r.seq,
        r.detected ? "true" : "false",
        r.global_score,
        (unsigned long)r.fps_1s,
        (unsigned)r.src_width, (unsigned)r.src_height);
    if (n < 0 || (size_t)n >= buflen) return 0;

    for (uint32_t i = 0; i < r.track_count; i++) {
        const motion_track_out_t *tr = &r.tracks[i];
        int m = snprintf(buf + n, buflen - (size_t)n,
            "%s{\"id\":%u,\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,"
            "\"s\":%.3f,\"age\":%lu,\"hits\":%lu,\"lost\":%s}",
            (i == 0) ? "" : ",",
            (unsigned)tr->id,
            (unsigned)tr->x, (unsigned)tr->y,
            (unsigned)tr->w, (unsigned)tr->h,
            tr->score,
            (unsigned long)tr->age_frames,
            (unsigned long)tr->hit_count,
            tr->lost ? "true" : "false");
        if (m < 0 || (size_t)(n + m) >= buflen) return 0;
        n += m;
    }

    int m = snprintf(buf + n, buflen - (size_t)n, "]}");
    if (m < 0 || (size_t)(n + m) >= buflen) return 0;
    n += m;
    return (size_t)n;
}
