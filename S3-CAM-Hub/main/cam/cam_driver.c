/**
 * @file cam_driver.c
 * @brief OV3660 camera hardware initialization.
 */
#include "cam_driver.h"
#include "cam_config.h"
#include "ov3660_s3_n16r8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

static const char *TAG = "cam_drv";
static bool s_initialized = false;
static char s_sccb_scan_result[80] = "scan not run";

esp_err_t cam_driver_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Camera already initialized.");
        return ESP_OK;
    }

    camera_config_t cfg = {
        /* Clock */
        .ledc_channel  = CAM_LEDC_CHANNEL,
        .ledc_timer    = CAM_LEDC_TIMER,
        .xclk_freq_hz  = BOARD_CAM_XCLK_MHZ * 1000000,

        /* SCCB */
        .pin_sscb_sda  = BOARD_CAM_SIOD_GPIO,
        .pin_sscb_scl  = BOARD_CAM_SIOC_GPIO,

        /* Power / reset */
        .pin_pwdn      = BOARD_CAM_PWDN_GPIO,
        .pin_reset     = BOARD_CAM_RESET_GPIO,

        /* Parallel data bus */
        .pin_d0        = BOARD_CAM_D0_GPIO,
        .pin_d1        = BOARD_CAM_D1_GPIO,
        .pin_d2        = BOARD_CAM_D2_GPIO,
        .pin_d3        = BOARD_CAM_D3_GPIO,
        .pin_d4        = BOARD_CAM_D4_GPIO,
        .pin_d5        = BOARD_CAM_D5_GPIO,
        .pin_d6        = BOARD_CAM_D6_GPIO,
        .pin_d7        = BOARD_CAM_D7_GPIO,

        /* Sync */
        .pin_vsync     = BOARD_CAM_VSYNC_GPIO,
        .pin_href      = BOARD_CAM_HREF_GPIO,
        .pin_pclk      = BOARD_CAM_PCLK_GPIO,

        /* Frame buffers — allocated in PSRAM */
        .pixel_format  = CAM_PIXEL_FORMAT,
        .frame_size    = CAM_FRAME_SIZE,
        .jpeg_quality  = CAM_JPEG_QUALITY,
        .fb_count      = CAM_FB_COUNT,
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .grab_mode     = CAM_GRAB_MODE,
    };

    /* ── SCCB diagnostic scan ────────────────────────────────────────────────
     * OV3660 requires XCLK to respond on SCCB. Start XCLK first, wait for
     * sensor stabilization, then scan. This identifies whether GPIO4/5 is
     * correct or a different pin pair is needed for this board.
     */
    {
        /* Start XCLK on BOARD_CAM_XCLK_GPIO so OV3660 clocks up */
        ledc_timer_config_t xclk_timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .timer_num       = CAM_LEDC_TIMER,
            .duty_resolution = LEDC_TIMER_1_BIT,
            .freq_hz         = (uint32_t)BOARD_CAM_XCLK_MHZ * 1000000U,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&xclk_timer);

        ledc_channel_config_t xclk_ch = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = CAM_LEDC_CHANNEL,
            .timer_sel  = CAM_LEDC_TIMER,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = BOARD_CAM_XCLK_GPIO,
            .duty       = 1,   /* 50% duty */
            .hpoint     = 0,
        };
        ledc_channel_config(&xclk_ch);

        /* Wait for sensor to stabilize with XCLK running */
        vTaskDelay(pdMS_TO_TICKS(200));

        /* Scan SCCB bus (configured SIOD/SIOC pins) */
        i2c_master_bus_config_t scan_bus_cfg = {
            .i2c_port          = I2C_NUM_1,
            .sda_io_num        = BOARD_CAM_SIOD_GPIO,
            .scl_io_num        = BOARD_CAM_SIOC_GPIO,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_master_bus_handle_t scan_bus;
        esp_err_t scan_init = i2c_new_master_bus(&scan_bus_cfg, &scan_bus);
        if (scan_init == ESP_OK) {
            ESP_LOGI(TAG, "SCCB scan (XCLK on GPIO%d) — SDA=%d SCL=%d:",
                     BOARD_CAM_XCLK_GPIO, BOARD_CAM_SIOD_GPIO, BOARD_CAM_SIOC_GPIO);
            int found = 0;
            for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
                if (i2c_master_probe(scan_bus, addr, 20) == ESP_OK) {
                    ESP_LOGI(TAG, "  Device at 0x%02X%s", addr,
                             addr == 0x3C ? " <- OV3660" : "");
                    snprintf(s_sccb_scan_result, sizeof(s_sccb_scan_result),
                             "found 0x%02X on SDA=%d SCL=%d",
                             addr, BOARD_CAM_SIOD_GPIO, BOARD_CAM_SIOC_GPIO);
                    found++;
                }
            }
            if (!found) {
                ESP_LOGW(TAG, "  Nothing on GPIO%d/GPIO%d. SCCB pins are wrong.",
                         BOARD_CAM_SIOD_GPIO, BOARD_CAM_SIOC_GPIO);
                snprintf(s_sccb_scan_result, sizeof(s_sccb_scan_result),
                         "NOTHING on SDA=%d SCL=%d (XCLK running)",
                         BOARD_CAM_SIOD_GPIO, BOARD_CAM_SIOC_GPIO);
            }
            i2c_del_master_bus(scan_bus);
        } else {
            ESP_LOGW(TAG, "SCCB scan init failed: %s", esp_err_to_name(scan_init));
        }

        /* XCLK intentionally left running — OV3660 needs >10ms of XCLK before
         * responding to SCCB. esp_camera_init() will reconfigure the same
         * LEDC timer/channel (safe to reconfigure while running). */
    }

    ESP_LOGI(TAG, "Initializing OV3660 camera ...");
    esp_err_t ret = esp_camera_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "SCCB diagnostic: %s", s_sccb_scan_result);
        ESP_LOGE(TAG, "Check: pin mapping in boards/ov3660_s3_n16r8.h");
        return ret;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        ESP_LOGE(TAG, "Camera sensor handle is NULL after init.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera sensor detected: PID=0x%04x", sensor->id.PID);

    /* OV3660-specific initial adjustments */
    sensor->set_vflip(sensor, 1);        /* Vertical flip for typical board orientation */
    sensor->set_brightness(sensor, 1);   /* Slight brightness boost */
    sensor->set_saturation(sensor, -2);  /* Reduce saturation for cleaner image */

    s_initialized = true;
    ESP_LOGI(TAG, "Camera ready. Format: JPEG, Frame size: %d, Quality: %d, FB count: %d",
             CAM_FRAME_SIZE, CAM_JPEG_QUALITY, CAM_FB_COUNT);
    return ESP_OK;
}

esp_err_t cam_driver_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    esp_err_t ret = esp_camera_deinit();
    if (ret == ESP_OK) {
        s_initialized = false;
        ESP_LOGI(TAG, "Camera deinitialized.");
    }
    return ret;
}

bool cam_driver_is_ready(void)
{
    return s_initialized;
}

sensor_t *cam_driver_get_sensor(void)
{
    if (!s_initialized) return NULL;
    return esp_camera_sensor_get();
}
