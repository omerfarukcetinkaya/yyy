/**
 * @file wifi_manager.h
 * @brief Wi-Fi station manager with automatic reconnection.
 *
 * Treats Wi-Fi as an always-on subsystem. Handles connect/disconnect events
 * internally and notifies other subsystems via an EventGroup.
 *
 * The HTTP server is started by wifi_manager automatically upon first IP
 * assignment and stopped on Wi-Fi loss (so it can rebind on reconnect).
 */
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/** EventGroup bits published by wifi_manager */
#define WIFI_BIT_CONNECTED   BIT0  /**< Associated and authenticated */
#define WIFI_BIT_GOT_IP      BIT1  /**< IP address assigned */

/**
 * @brief Initialize Wi-Fi in station mode and begin connection.
 *
 * Sets up the default event loop, netif, and Wi-Fi stack. Credentials
 * come from Kconfig (VH_WIFI_SSID / VH_WIFI_PASSWORD).
 *
 * @return ESP_OK on successful start (not necessarily connected yet).
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Get the EventGroup handle (for other modules to wait on WIFI_BIT_GOT_IP).
 * Returns NULL if wifi_manager_init() has not been called.
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

/**
 * @brief Get current IP address string (e.g. "192.168.1.42").
 * Returns "0.0.0.0" if not connected.
 */
const char *wifi_manager_get_ip(void);

/**
 * @brief Get current RSSI in dBm. Returns 0 if not connected.
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief Get total reconnect attempt count since boot.
 */
uint32_t wifi_manager_get_reconnect_count(void);

/**
 * @brief Return true if Wi-Fi has an IP address currently.
 */
bool wifi_manager_is_connected(void);
