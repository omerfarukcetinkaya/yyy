/**
 * @file roi_tracker.h
 * @brief Region of Interest (ROI) tracking interface.
 *
 * Phase 1: STUB.
 * Phase 4+: Track ROI bounding boxes across frames, smooth trajectories,
 *           detect entry/exit events.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint16_t x, y, w, h;
    float    confidence;
    uint32_t track_id;
    uint32_t age_frames;   /**< Frames since first seen */
} roi_track_t;

#define ROI_MAX_TRACKS  8

typedef struct {
    roi_track_t tracks[ROI_MAX_TRACKS];
    uint8_t     count;
} roi_result_t;

esp_err_t roi_tracker_init(void);
void roi_tracker_update(uint16_t x, uint16_t y, uint16_t w, uint16_t h, float conf);
void roi_tracker_get_result(roi_result_t *out);
void roi_tracker_reset(void);
