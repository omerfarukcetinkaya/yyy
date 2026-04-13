/**
 * @file rgb_led.c
 * @brief WS2812 RGB LED control on ESP32-C5 DevKit (GPIO 27).
 *
 * LED behavior:
 *   - Boot: brief blue flash then OFF
 *   - Normal: OFF (no light pollution)
 *   - Alarm active: solid RED
 *   - Alarm cleared: OFF
 */
#include "rgb_led.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "rgb_led";

#define RGB_LED_GPIO    27
#define RGB_LED_COUNT   1

static led_strip_handle_t s_strip = NULL;

esp_err_t rgb_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start OFF */
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "RGB LED initialized (GPIO %d) — OFF", RGB_LED_GPIO);
    return ESP_OK;
}

void rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void rgb_led_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
}

void rgb_led_alarm(bool active)
{
    if (active) {
        rgb_led_set(40, 0, 0);  /* dim red — not blinding */
    } else {
        rgb_led_off();
    }
}
