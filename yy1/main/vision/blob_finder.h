/**
 * @file blob_finder.h
 * @brief 4-connected component labeling on a binary motion mask.
 *
 * Given a mask of width × height bytes where non-zero pixels are
 * "foreground" (motion), produces up to MAX_BLOBS blob descriptors:
 *   - Axis-aligned bounding box (x, y, w, h) in mask coordinates
 *   - Pixel area
 *   - Sum of original mask intensities (used as motion score proxy)
 *
 * Algorithm: two-pass union-find.
 *   Pass 1: scan pixels, assign labels based on left/top neighbor;
 *           when both neighbors have different labels, union them.
 *   Pass 2: flatten union-find, accumulate per-root stats.
 *
 * The output is sorted by descending area and filtered by a minimum area
 * threshold to reject noise. Two moving hands separated by background
 * yield two separate blobs; a single person yields one large blob.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BLOB_MAX_COUNT        8u     /* Caller-visible top-N blobs */

typedef struct {
    uint16_t x, y, w, h;      /* Bounding box in mask coordinates */
    uint32_t area;            /* Pixel count */
    uint32_t intensity_sum;   /* Sum of mask byte values inside the blob */
    float    score;           /* Normalized 0..1 — intensity_sum / (area*255) */
} blob_t;

typedef struct {
    blob_t   blobs[BLOB_MAX_COUNT];
    uint32_t count;
    uint32_t total_fg_pixels;   /* Diagnostic: total foreground pre-filtering */
    uint32_t total_components;  /* Diagnostic: total components pre-filtering */
} blob_result_t;

/**
 * @brief Run connected-component labeling on a mask.
 *
 * @param mask         Byte mask, 0 = background, >0 = foreground intensity.
 * @param width        Mask width (must be ≤ 320).
 * @param height       Mask height (must be ≤ 240).
 * @param min_area     Blobs smaller than this are discarded.
 * @param labels       Scratch buffer, width*height uint16_t (caller-owned).
 * @param out          Filled with top-N blobs sorted by area.
 */
void blob_finder_run(const uint8_t *mask,
                     uint16_t       width,
                     uint16_t       height,
                     uint32_t       min_area,
                     uint16_t      *labels,
                     blob_result_t *out);
