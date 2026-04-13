idf_component_register(
    SRCS
        "app_main.c"
        "net/wifi_dual.c"
        "net/telegram_client.c"
        "net/espnow_bridge.c"
        "system/watchdog.c"
        "system/status_reporter.c"

    INCLUDE_DIRS
        "."
        "net"
        "system"

    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        nvs_flash
        esp_timer
        freertos
        log
)
