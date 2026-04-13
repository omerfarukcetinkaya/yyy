/**
 * @file cam_config.h
 * @brief Camera pipeline constants.
 *
 * All camera-specific tuning knobs live here. Values here may be
 * overridden by Kconfig (VH_CAM_* symbols) where appropriate.
 */
#pragma once
#include "esp_camera.h"
#include "sdkconfig.h"

/* ── Output format ────────────────────────────────────────────────────────── */
/* OV3660 in JPEG mode: sensor encodes onchip, zero-copy to frame buffer.    */
#define CAM_PIXEL_FORMAT    PIXFORMAT_JPEG

/* ── Resolution ───────────────────────────────────────────────────────────── */
/* FRAMESIZE enum:                                                             */
/*   FRAMESIZE_QVGA   = 320×240                                               */
/*   FRAMESIZE_VGA    = 640×480   (good balance for 20 FPS @ Wi-Fi stream)   */
/*   FRAMESIZE_SVGA   = 800×600                                               */
/*   FRAMESIZE_HD     = 1280×720                                              */
/* Start at VGA; bump up once pipeline is validated.                          */
#define CAM_FRAME_SIZE      ((framesize_t)CONFIG_VH_CAM_FRAME_SIZE)

/* ── JPEG quality: 0=best quality/large file, 63=worst/small file ─────────── */
#define CAM_JPEG_QUALITY    CONFIG_VH_CAM_JPEG_QUALITY

/* ── Frame buffer count (allocated in PSRAM) ──────────────────────────────── */
#define CAM_FB_COUNT        CONFIG_VH_CAM_FB_COUNT

/* ── Grab mode ────────────────────────────────────────────────────────────────
 * CAMERA_GRAB_LATEST: DMA runs continuously; cam_task discards stale frames
 * and keeps only the latest in the queue. fb_get() is non-blocking when the
 * snapshot cycle (TCP send ~90 ms + client RTT) exceeds the VGA frame period
 * (40 ms at 25 fps) — the next frame is already queued before fb_get() runs.
 *
 * CAMERA_GRAB_WHEN_EMPTY must NOT be used for aperiodic (snapshot polling)
 * consumers: DMA stops permanently when all buffers are full and no fb_get()
 * is called. This causes the "frames counter frozen" symptom. */
#define CAM_GRAB_MODE       CAMERA_GRAB_LATEST

/* ── Pipeline timing ─────────────────────────────────────────────────────── */
/* Master FPS target. All pipeline timings derive from this one constant.
 * Change this and rebuild — MJPEG stream, motion, classifier all adapt.
 *
 * VGA (640×480) at PCLK=10 MHz → ~10 fps hardware maximum.
 * 10 fps × 70 KB JPEG = 700 KB/s = 5.6 Mbps — within Wi-Fi TCP budget.         */
#define CAM_TARGET_FPS              10
#define CAM_FRAME_PERIOD_MS         (1000U / CAM_TARGET_FPS)   /* 100 ms */
#define CAM_ACQUIRE_PERIOD_MS       CAM_FRAME_PERIOD_MS

/* Motion runs at 5 FPS (CAM_TARGET_FPS/3 = 5).
 * Enough for human-motion detection; low enough to not compete with stream. */
#define MOTION_SAMPLE_RATE_FPS      (CAM_TARGET_FPS / 3)   /* = 5 FPS */
#define MOTION_SAMPLE_PERIOD_MS     (1000U / MOTION_SAMPLE_RATE_FPS)  /* 200 ms */

/* Classifier runs at 1 Hz regardless of stream fps */
#define CLASSIFIER_SAMPLE_RATE_FPS  1
#define CLASSIFIER_SAMPLE_PERIOD_MS (1000U / CLASSIFIER_SAMPLE_RATE_FPS)

/* ── LEDC channel / timer for XCLK generation ────────────────────────────── */
#define CAM_LEDC_CHANNEL    LEDC_CHANNEL_0
#define CAM_LEDC_TIMER      LEDC_TIMER_0
