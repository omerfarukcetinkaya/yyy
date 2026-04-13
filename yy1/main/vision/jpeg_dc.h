/**
 * @file jpeg_dc.h
 * @brief Extract DC luminance coefficients directly from a JPEG bitstream.
 *
 * A full JPEG decode (IDCT + color conversion + pixel write) takes
 * ~200 ms on ESP32-S3 for VGA→80×60. This module extracts only the
 * DC coefficient of each 8×8 Y block by Huffman-decoding the scan
 * data and skipping all AC coefficients and chrominance channels.
 *
 * For VGA (640×480) with 4:2:2 or 4:2:0 chroma subsampling, the Y
 * DC grid is exactly 80×60 — one value per 8×8 source block. Each
 * value is dequantized (DC * Q[0]) and shifted to 0..255 range,
 * producing an 80×60 "thumbnail" of average block luminance. This is
 * fed directly to the motion diff pipeline with zero IDCT cost.
 *
 * Expected throughput: ~3–8 ms per VGA frame at 240 MHz.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    uint8_t  *dc_y;     /**< Caller-allocated buffer, dc_w × dc_h bytes */
    uint16_t  dc_w;     /**< Columns in the DC grid (filled on success) */
    uint16_t  dc_h;     /**< Rows in the DC grid */
} jpeg_dc_result_t;

/**
 * @brief Parse JPEG headers and extract Y-channel DC coefficients.
 *
 * @param jpeg      JPEG bitstream (starts with FFD8).
 * @param jpeg_len  Length in bytes.
 * @param out       Result struct. dc_y must point to a buffer of at
 *                  least (image_width/8) × (image_height/8) bytes.
 *                  dc_w and dc_h are written on success.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad input,
 *         ESP_FAIL on parse error (corrupt or unsupported JPEG).
 */
esp_err_t jpeg_extract_dc_luma(const uint8_t *jpeg, size_t jpeg_len,
                               jpeg_dc_result_t *out);
