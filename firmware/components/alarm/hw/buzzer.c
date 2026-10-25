#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "fx_alarm_hw.h"

/* The fan owns LEDC timer 0 and channel 0 at 25 kHz; the buzzer takes timer 1. */
#define BUZZER_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER LEDC_TIMER_1
#define BUZZER_CHANNEL LEDC_CHANNEL_1
#define BUZZER_RES LEDC_TIMER_10_BIT

static const char *TAG = "buzzer";

esp_err_t fx_buzzer_init(fx_buzzer_t *b, int gpio, fx_buzzer_kind_t kind, uint32_t tone_hz)
{
    ESP_RETURN_ON_FALSE(b && gpio >= 0, ESP_ERR_INVALID_ARG, TAG, "gpio required");
    *b = (fx_buzzer_t){.gpio = gpio, .kind = kind};

    if (kind == FX_BUZZER_PASSIVE) {
        const ledc_timer_config_t timer = {
            .speed_mode = BUZZER_MODE,
            .duty_resolution = BUZZER_RES,
            .timer_num = BUZZER_TIMER,
            .freq_hz = tone_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
        const ledc_channel_config_t channel = {
            .gpio_num = gpio,
            .speed_mode = BUZZER_MODE,
            .channel = BUZZER_CHANNEL,
            .timer_sel = BUZZER_TIMER,
            .duty = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");
    } else {
        const gpio_config_t io = {.pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
        ESP_RETURN_ON_ERROR(gpio_set_level(gpio, 0), TAG, "gpio level");
    }
    b->ready = true;
    return ESP_OK;
}

esp_err_t fx_buzzer_set(fx_buzzer_t *b, bool on)
{
    if (!b || !b->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == b->on) {
        return ESP_OK;
    }
    esp_err_t err;
    if (b->kind == FX_BUZZER_PASSIVE) {
        err = ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, on ? (1u << BUZZER_RES) / 2u : 0u);
        if (err == ESP_OK) {
            err = ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
        }
    } else {
        err = gpio_set_level(b->gpio, on ? 1 : 0);
    }
    if (err == ESP_OK) {
        b->on = on;
    }
    return err;
}
