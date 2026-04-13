idf_component_register(
    SRCS
        "app_main.c"

        # System layer
        "system/fault_handler.c"
        "system/system_monitor.c"
        "system/watchdog.c"

        # Camera layer
        "cam/cam_driver.c"
        "cam/cam_pipeline.c"
        "cam/frame_pool.c"

        # Vision layer
        "vision/motion_detect.c"
        "vision/blob_finder.c"
        "vision/jpeg_dc.c"
        "vision/face_detect.cpp"
        "vision/roi_tracker.c"
        "vision/classifier.c"

        # Network layer
        "net/wifi_manager.c"
        "net/http_server.c"
        "net/stream_handler.c"
        "net/ws_stream.c"
        "net/web_auth.c"
        "net/admin_panel.c"

        # Sensor layer
        "sensors/i2c_bus.c"
        "sensors/adc_bus.c"
        "sensors/sensor_hub.c"
        "sensors/aht20.c"
        "sensors/bmp280.c"
        "sensors/mq7.c"
        "sensors/mq137.c"
        "sensors/veml7700.c"

        # Alarm layer
        "alarm/alarm_engine.c"
        "alarm/gas_analyzer.c"
        "alarm/telegram_transport.c"

        # Display layer
        "display/oled_display.c"

        # Storage layer
        "storage/ext_flash.c"
        "storage/bist_logger.c"

        # Telemetry layer
        "telemetry/telemetry_report.c"
        "telemetry/uart_reporter.c"
        "telemetry/web_reporter.c"

    INCLUDE_DIRS
        "."
        "system"
        "cam"
        "vision"
        "net"
        "sensors"
        "alarm"
        "display"
        "storage"
        "telemetry"
        "../boards"

    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        nvs_flash
        driver
        esp_timer
        esp32-camera
        espressif__esp-dl
        espressif__human_face_detect
        espressif__human_face_recognition
        freertos
        log
        heap
        mbedtls
        esp_adc
        spi_flash
)
