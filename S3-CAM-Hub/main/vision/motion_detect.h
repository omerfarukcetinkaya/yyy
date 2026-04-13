/**
 * @file motion_detect.h
 * @brief High-quality motion detection with persistent multi-object tracking.
 *
 * Pipeline (Core 1, stride-paced to ≥4 fps):
 *   1. frame_pool_wait_newer() → latest JPEG slot
 *   2. jpg2rgb565(JPG_SCALE_8X) → 80×60 RGB565
 *   3. RGB565 → grayscale
 *   4. abs-diff vs previous grayscale, threshold → binary/intensity mask
 *   5. 3×3 cross dilate on the mask (closes 1-pixel gaps within a moving
 *      body so ears/hair/eyes/mouth merge into one head blob)
 *   6. 4-connectivity CC labeling → raw blobs
 *   7. Blob proximity merge (bbox gap ≤ N mask pixels → union)
 *   8. Temporal tracker: match merged blobs against existing tracks by
 *      centroid distance; smooth bbox/score; spawn new tracks; hold lost
 *      tracks for up to MOTION_TRACK_HOLD_MISSES frames before dropping
 *   9. Publish only confirmed tracks (hits ≥ CONFIRM_HITS)
 *  10. Feed alarm_engine with (any confirmed) and max smoothed score
 *
 * UI behavior:
 *   - A new motion region becomes visible after CONFIRM_HITS consecutive
 *     matches (prevents noise flicker).
 *   - Once confirmed, the box stays visible for HOLD_MISSES frames even
 *     after the underlying motion vanishes — the track is "sticky", so
 *     the user sees a solid box until motion is truly over.
 *   - Each track has a stable integer id. The UI can render id + age.
 *
 * Multiple people / two hands moving separately produce distinct tracks
 * because the dilate + proximity-merge logic only consolidates
 * CONTIGUOUS fragmented pieces, not spatially separated regions.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MOTION_MAX_TRACKS    6u

typedef enum {
    TRACK_STATE_TRACKING  = 0,   /**< Actively matching blobs — green */
    TRACK_STATE_ALERT     = 1,   /**< Sustained high-score motion — red, alarm */
    TRACK_STATE_POSSIBLE  = 2,   /**< Motion stopped but threat not cleared — yellow */
} motion_track_state_t;

typedef struct {
    uint16_t id;              /**< Stable per-track identifier */
    uint16_t x, y, w, h;      /**< Bounding box in SOURCE frame coords */
    float    score;           /**< Smoothed intensity score [0..1] */
    uint32_t age_frames;      /**< Frames since first seen */
    uint32_t hit_count;       /**< Number of frames this track matched a blob */
    motion_track_state_t state; /**< Visual/alarm state */
} motion_track_out_t;

typedef struct {
    uint32_t            seq;
    int64_t             ts_us;
    bool                detected;      /**< Any confirmed track present */
    float               global_score;  /**< Max smoothed score among tracks */
    uint32_t            track_count;   /**< Number of visible tracks */
    motion_track_out_t  tracks[MOTION_MAX_TRACKS];
    uint16_t            src_width;
    uint16_t            src_height;
    uint32_t            fps_1s;
} motion_result_t;

/** @brief Allocate buffers (DRAM + PSRAM) and init mutex. */
esp_err_t motion_detect_init(void);

/** @brief Start the motion task on Core 1. */
esp_err_t motion_detect_start(void);

/** @brief Thread-safe snapshot of the latest motion result. */
void motion_detect_get_result(motion_result_t *out);

/**
 * @brief Serialize the latest result as JSON into a caller-provided buffer.
 * Format:
 *   {"t":"motion","seq":N,"det":bool,"score":F,"fps":F,"w":W,"h":H,
 *    "tracks":[{"id":I,"x":..,"y":..,"w":..,"h":..,"s":..,"age":N,"lost":bool},...]}
 * Returns bytes written (not including NUL) or 0 on error/overflow.
 */
size_t motion_detect_build_json(char *buf, size_t buflen);
