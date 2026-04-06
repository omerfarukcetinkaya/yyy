/**
 * @file oled_display.h
 * @brief SSD1306 OLED 128x32 driver (I2C, address 0x3C).
 *
 * Provides a 4-line text interface using an embedded 6x8 pixel font
 * (21 chars per line, 4 lines on a 128x32 display).
 *
 * Display layout:
 *   Line 0: System status / IP address
 *   Line 1: Temperature, Humidity, Pressure
 *   Line 2: CO, NH3, Lux
 *   Line 3: Gas status, Alarm indicator
 */
#pragma once
#include "esp_err.h"
#include "telemetry_report.h"

/** @brief Initialize SSD1306 and clear the screen. */
esp_err_t oled_display_init(void);

/**
 * @brief Refresh the 4-line status display from a telemetry snapshot.
 * Writes all four lines and flushes the framebuffer to the OLED.
 */
void oled_display_update(const telemetry_t *t);

/** @brief Write a raw text string to a display line (0-3). */
void oled_display_set_line(uint8_t line, const char *text);

/** @brief Clear the display. */
void oled_display_clear(void);
