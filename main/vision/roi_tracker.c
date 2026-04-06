/**
 * @file roi_tracker.c
 * @brief ROI tracker — Phase 1 STUB.
 * TODO Phase 4: Implement centroid tracking with IoU matching.
 */
#include "roi_tracker.h"
#include <string.h>

static roi_result_t s_result;

esp_err_t roi_tracker_init(void)  { memset(&s_result, 0, sizeof(s_result)); return ESP_OK; }
void roi_tracker_update(uint16_t x, uint16_t y, uint16_t w, uint16_t h, float conf) { (void)x;(void)y;(void)w;(void)h;(void)conf; }
void roi_tracker_get_result(roi_result_t *out) { if (out) memcpy(out, &s_result, sizeof(*out)); }
void roi_tracker_reset(void) { memset(&s_result, 0, sizeof(s_result)); }
