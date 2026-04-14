/**
 * @file sensor_registry.h
 * @brief Generic registry of ESP32 sensors on the 2.4G IoT mesh.
 *
 * Each sensor is a networked device reachable via HTTP (or ESP-NOW
 * in future) with a telemetry endpoint. Scout polls each sensor on a
 * schedule, parses its status JSON, and aggregates health info for:
 *   - /status Telegram command
 *   - BIST 15-min heartbeat
 *   - Admin panel
 *
 * S3 Vision Hub is registered as sensor 0. Add more by appending to
 * the static table in sensor_registry.c.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SENSOR_MAX            8
#define SENSOR_NAME_MAX      24
#define SENSOR_REASON_MAX    64
#define SENSOR_SUMMARY_MAX  128

typedef struct {
    /* Static config — set at boot, read-only during runtime. */
    const char *name;            /* "S3 Vision Hub" */
    const char *ip;              /* "192.168.39.157" */
    const char *telemetry_path;  /* "/api/telemetry" */
    const char *username;
    const char *password;

    /* Runtime state — updated by sensor_registry_poll_one(). */
    bool        online;
    int64_t     last_poll_us;
    int64_t     last_success_us;
    uint32_t    poll_ok;
    uint32_t    poll_fail;

    /* Common parsed fields — any sensor type may provide a subset. */
    uint32_t    uptime_s;
    uint32_t    cam_fps;          /* 0 if sensor has no camera */
    uint32_t    motion_fps;
    bool        motion_detected;
    float       motion_score;
    bool        alarm_active;
    char        alarm_reason[SENSOR_REASON_MAX];
    uint32_t    heap_free_b;
    uint32_t    psram_free_b;
    int8_t      rssi;
    uint8_t     cpu0_pct;
    uint8_t     cpu1_pct;
} sensor_t;

/** Initialize the static sensor table. Call once at boot. */
esp_err_t sensor_registry_init(void);

/** Start the periodic polling task (Core 0). */
esp_err_t sensor_registry_start(void);

/** Total registered sensors (static, never changes at runtime). */
int sensor_registry_count(void);

/** Online sensors right now. */
int sensor_registry_online_count(void);

/** Access a sensor by index (0 <= idx < sensor_registry_count()). */
const sensor_t *sensor_registry_get(int idx);

/** Write a single-line human summary of sensor `idx` into `buf`. */
size_t sensor_registry_format_line(int idx, char *buf, size_t buflen);

/** Write a multi-line full status block of all sensors. */
size_t sensor_registry_format_all(char *buf, size_t buflen);

/** Is any sensor currently reporting an active alarm? */
bool sensor_registry_any_alarm(void);

/** First sensor with active alarm (or NULL). */
const sensor_t *sensor_registry_first_alarm(void);
