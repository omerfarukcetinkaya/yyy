/**
 * @file telemetry_report.h
 * @brief Central telemetry struct and aggregation API.
 *
 * All subsystems write their metrics into a single global telemetry_t
 * protected by a mutex. The telemetry task reads a snapshot once per
 * second and dispatches it to uart_reporter and web_reporter.
 *
 * Design note: fields are updated by owner modules only. Readers always
 * take a snapshot via telemetry_snapshot() — never read fields directly
 * from the live struct to avoid partial reads.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /* ── System ──────────────────────────────────────────────────────── */
    uint32_t uptime_s;
    char     reset_reason[32];

    /* ── Wi-Fi ───────────────────────────────────────────────────────── */
    bool     wifi_connected;
    char     wifi_ip[16];
    int8_t   wifi_rssi_dbm;
    uint32_t wifi_reconnect_count;

    /* ── Camera pipeline ─────────────────────────────────────────────── */
    uint32_t cam_fps_1s;
    uint32_t cam_frame_count;
    uint32_t cam_drop_count;

    /* ── Vision pipeline ─────────────────────────────────────────────── */
    uint32_t motion_fps_1s;
    bool     motion_detected;
    float    motion_score;
    uint32_t classifier_fps_1s;
    char     classifier_label[32];
    float    classifier_confidence;

    /* ── Stream ──────────────────────────────────────────────────────── */
    uint32_t stream_client_count;

    /* ── Sensors ─────────────────────────────────────────────────────── */
    float    temp_c;
    float    humidity_pct;
    float    pressure_hpa;
    float    co_ppm;
    float    nh3_ppm;
    float    lux;
    uint8_t  gas_status;            /* gas_status_t enum value */
    char     gas_status_str[16];    /* human-readable: NORMAL, FIRE, etc. */
    uint32_t sensor_read_count;
    uint32_t sensor_error_count;

    /* ── CPU ────────────────────────────────────────────────────────────── */
    uint8_t  cpu_core0_pct;       /**< Core 0 busy % (delta since last snapshot) */
    uint8_t  cpu_core1_pct;       /**< Core 1 busy % */

    /* ── Memory ──────────────────────────────────────────────────────── */
    uint32_t heap_free_b;
    uint32_t heap_free_min_b;
    uint32_t heap_largest_block_b;
    uint32_t internal_free_b;
    uint32_t psram_free_b;
    uint32_t psram_free_min_b;
    uint32_t psram_largest_block_b;

    /* ── Alarm ───────────────────────────────────────────────────────── */
    bool     alarm_active;
    uint32_t alarm_count;
    char     alarm_last_reason[64];
    uint32_t telegram_sent_count;
    uint32_t telegram_fail_count;

    /* ── Error counters ──────────────────────────────────────────────── */
    uint32_t err_cam_init;
    uint32_t err_cam_fb_timeout;
    uint32_t err_wifi_fail;
    uint32_t err_sensor_read;

    /* ── Timestamp ───────────────────────────────────────────────────── */
    int64_t  last_update_us;
} telemetry_t;

/**
 * @brief Initialize the telemetry module and its mutex.
 * Call once from app_main().
 */
void telemetry_init(void);

/**
 * @brief Take a mutex-protected snapshot of the current telemetry state.
 * The caller receives a fully consistent copy.
 */
void telemetry_snapshot(telemetry_t *out);

/**
 * @brief Update telemetry fields with a partial struct.
 * Acquires mutex, merges the provided fields, releases mutex.
 * Pass a telemetry_t with only the fields you want to update set.
 * Use telemetry_update_* helpers for type-safe updates.
 */
void telemetry_update(const telemetry_t *partial);

/* ── Type-safe field updaters ─────────────────────────────────────────────── */
void telemetry_set_wifi(bool connected, const char *ip, int8_t rssi, uint32_t reconnects);
void telemetry_set_camera(uint32_t fps, uint32_t frames, uint32_t drops);
void telemetry_set_motion(uint32_t fps, bool detected, float score);
void telemetry_set_classifier(uint32_t fps, const char *label, float confidence);
void telemetry_set_sensors(float temp_c, float humidity, float pressure,
                           float co_ppm, float nh3_ppm, float lux,
                           uint8_t gas_status, const char *gas_status_str,
                           uint32_t reads, uint32_t errors);
void telemetry_set_alarm(bool active, uint32_t count, const char *reason,
                         uint32_t tg_sent, uint32_t tg_fail);
void telemetry_set_stream_clients(uint32_t count);
void telemetry_refresh_system(void);  /**< Updates heap, uptime, reset_reason */
void telemetry_increment_cam_init_error(void); /**< Increment err_cam_init counter */
