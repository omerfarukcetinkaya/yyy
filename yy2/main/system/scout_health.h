/**
 * @file scout_health.h
 * @brief Scout self-diagnostics: CPU load, memory, reset reason, uptime, WDT.
 *
 * Updated by a background task every second. Reader must take the mutex
 * via scout_health_snapshot() to get a consistent copy.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t uptime_s;
    uint32_t heap_free_b;
    uint32_t heap_min_free_b;       /* lowest ever */
    uint32_t heap_largest_block_b;
    uint32_t internal_free_b;       /* DRAM free */
    uint8_t  cpu_pct;               /* aggregate CPU busy % (100 - idle) */
    uint32_t task_count;
    const char *reset_reason;       /* human string */
    uint32_t reboot_count;          /* persisted in NVS across boots */
    uint32_t wdt_timeouts;          /* TWDT triggers since boot */
    bool     wifi_connected;
    bool     wifi_on_5g;
    char     wifi_ip[16];
    int8_t   wifi_rssi;
    uint32_t tg_poll_ok;
    uint32_t tg_poll_fail;
    uint32_t tg_sent_ok;
    uint32_t tg_sent_fail;
    bool     tg_muted;
} scout_health_t;

esp_err_t scout_health_init(void);
esp_err_t scout_health_start(void);
void scout_health_snapshot(scout_health_t *out);

/* Telegram client may optionally increment these for diagnostics */
void scout_health_inc_tg_poll_ok(void);
void scout_health_inc_tg_poll_fail(void);
void scout_health_inc_tg_sent_ok(void);
void scout_health_inc_tg_sent_fail(void);

/**
 * @brief Should we send a boot notification?
 * Returns true if at least BOOT_NOTIFY_MIN_INTERVAL seconds have passed
 * since the last one (tracked in NVS). Prevents boot-loop message spam.
 */
bool scout_health_should_send_boot_notification(void);

/** Mark boot notification sent (updates NVS timestamp). */
void scout_health_mark_boot_notification_sent(void);
