/**
 * @file face_detect.h
 * @brief C interface for ESP-DL face detection + recognition on ESP32-S3.
 *
 * Wraps the C++ espressif/human_face_detect and human_face_recognition
 * managed components. The module runs a vision task (Core 1, prio 3)
 * that processes motion-triggered JPEG frames from the frame pool.
 *
 * Two operation modes:
 *   ENROLLMENT: first N detected faces are enrolled as owner ("Ömer").
 *   GUARD:      detected faces are compared against enrolled embeddings.
 *               Match → "Ömer" (green label). No match → "Unknown" (red alarm).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_RESULTS   4

typedef struct {
    uint16_t x, y, w, h;    /**< Bounding box in source frame coords */
    float    score;          /**< Detection confidence 0..1 */
    int16_t  id;             /**< Recognition ID (>0 = enrolled, -1 = unknown, 0 = not recognized) */
    float    similarity;     /**< Recognition similarity 0..1 (valid when id > 0) */
    char     name[16];       /**< Recognized name or "Unknown" */
} face_result_t;

typedef struct {
    uint32_t     seq;
    uint32_t     face_count;
    face_result_t faces[FACE_MAX_RESULTS];
    uint32_t     fps_1s;
    uint32_t     enrolled_count;     /**< Number of enrolled faces in DB */
    bool         enrollment_mode;    /**< True if still in enrollment mode */
} face_detect_result_t;

/** @brief Initialize face detection models (loads to PSRAM). */
esp_err_t face_detect_init(void);

/** @brief Start the vision task on Core 1. */
esp_err_t face_detect_start(void);

/** @brief Thread-safe snapshot of latest face detection result. */
void face_detect_get_result(face_detect_result_t *out);

/** @brief Trigger enrollment of the next detected face. */
void face_detect_enroll_next(void);

/** @brief Clear all enrolled faces. */
esp_err_t face_detect_clear_enrollments(void);

/** @brief Build JSON for WS motion channel. */
size_t face_detect_build_json(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
