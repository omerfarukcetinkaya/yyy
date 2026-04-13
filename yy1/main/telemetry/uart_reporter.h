/**
 * @file uart_reporter.h
 * @brief 1 Hz UART telemetry report.
 * Formats a human-readable BIST-style health report to the serial console.
 */
#pragma once
#include "telemetry_report.h"

/** @brief Print one complete telemetry report to UART. */
void uart_reporter_print(const telemetry_t *telem);
