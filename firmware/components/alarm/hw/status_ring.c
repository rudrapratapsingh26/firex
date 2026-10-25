#include "esp_check.h"
#include "fx_alarm_hw.h"
#include "led_strip.h"

static const char *TAG = "ring";

esp_err_t fx_status_ring_init(fx_status_ring_t *r, int gpio, uint16_t count)
{
    ESP_RETURN_ON_FALSE(r && gpio >= 0 && count > 0, ESP_ERR_INVALID_ARG, TAG, "gpio and count required");
    *r = (fx_status_ring_t){.count = count};

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds = count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    led_strip_handle_t strip = NULL;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip), TAG, "led_strip");
    r->strip = strip;
    ESP_RETURN_ON_ERROR(led_strip_clear(strip), TAG, "clear");
    r->ready = true;
    return ESP_OK;
}

esp_err_t fx_status_ring_show(fx_status_ring_t *r, fx_rgb_t colour)
{
    if (!r || !r->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (r->have_shown && r->shown.r == colour.r && r->shown.g == colour.g && r->shown.b == colour.b) {
        return ESP_OK;
    }
    led_strip_handle_t strip = (led_strip_handle_t)r->strip;
    for (uint16_t i = 0; i < r->count; i++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(strip, i, colour.r, colour.g, colour.b), TAG, "pixel");
    }
    ESP_RETURN_ON_ERROR(led_strip_refresh(strip), TAG, "refresh");
    r->shown = colour;
    r->have_shown = true;
    return ESP_OK;
}
