#include "fx_fan.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"

#define FAN_PWM_HZ 25000
#define FAN_RES LEDC_TIMER_10_BIT
#define FAN_MODE LEDC_LOW_SPEED_MODE
#define FAN_TIMER LEDC_TIMER_0
#define FAN_CHANNEL LEDC_CHANNEL_0
#define TACH_WINDOW_MS 1000u

static const char *TAG = "fan";

esp_err_t fx_fan_init(fx_fan_t *fan, int pwm_gpio, int tach_gpio, uint32_t now_ms)
{
    ESP_RETURN_ON_FALSE(fan && pwm_gpio >= 0, ESP_ERR_INVALID_ARG, TAG, "pwm gpio required");
    *fan = (fx_fan_t){.pwm_gpio = pwm_gpio, .tach_gpio = tach_gpio, .command_ms = now_ms,
                      .window_start_ms = now_ms};
    portMUX_INITIALIZE(&fan->lock);
    fx_fan_defaults(&fan->params);

    const ledc_timer_config_t timer = {
        .speed_mode = FAN_MODE,
        .duty_resolution = FAN_RES,
        .timer_num = FAN_TIMER,
        .freq_hz = FAN_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t channel = {
        .gpio_num = pwm_gpio,
        .speed_mode = FAN_MODE,
        .channel = FAN_CHANNEL,
        .timer_sel = FAN_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");

    if (tach_gpio >= 0) {
        const pcnt_unit_config_t unit_cfg = {.low_limit = -1, .high_limit = 32767};
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &fan->pcnt), TAG, "pcnt unit");
        const pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
        ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(fan->pcnt, &filter), TAG, "pcnt filter");
        const pcnt_chan_config_t chan_cfg = {.edge_gpio_num = tach_gpio, .level_gpio_num = -1};
        pcnt_channel_handle_t chan = NULL;
        ESP_RETURN_ON_ERROR(pcnt_new_channel(fan->pcnt, &chan_cfg, &chan), TAG, "pcnt channel");
        ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                         PCNT_CHANNEL_EDGE_ACTION_HOLD),
                            TAG, "pcnt edges");
        gpio_pullup_en(tach_gpio); /* the tachometer output is open collector */
        ESP_RETURN_ON_ERROR(pcnt_unit_enable(fan->pcnt), TAG, "pcnt enable");
        ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(fan->pcnt), TAG, "pcnt clear");
        ESP_RETURN_ON_ERROR(pcnt_unit_start(fan->pcnt), TAG, "pcnt start");
    }
    fan->ready = true;
    return ESP_OK;
}

esp_err_t fx_fan_set_duty(fx_fan_t *fan, uint8_t duty_pct, uint32_t now_ms)
{
    ESP_RETURN_ON_FALSE(fan && fan->ready, ESP_ERR_INVALID_STATE, TAG, "fan not initialised");
    if (duty_pct > 100u) {
        duty_pct = 100u;
    }
    const uint32_t max = (1u << FAN_RES) - 1u;
    const uint32_t duty = (max * duty_pct + 50u) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(FAN_MODE, FAN_CHANNEL, duty), TAG, "set duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(FAN_MODE, FAN_CHANNEL), TAG, "update duty");
    portENTER_CRITICAL(&fan->lock);
    fan->duty_pct = duty_pct;
    fan->command_ms = now_ms;
    portEXIT_CRITICAL(&fan->lock);
    return ESP_OK;
}

void fx_fan_poll(fx_fan_t *fan, uint32_t now_ms)
{
    if (!fan || !fan->ready || !fan->pcnt) {
        return;
    }
    const uint32_t elapsed = now_ms - fan->window_start_ms;
    if (elapsed < TACH_WINDOW_MS) {
        return;
    }
    int count = 0;
    if (pcnt_unit_get_count(fan->pcnt, &count) != ESP_OK) {
        return;
    }
    pcnt_unit_clear_count(fan->pcnt);
    const uint32_t rpm = fx_fan_rpm(count > 0 ? (uint32_t)count : 0u, elapsed, fan->params.pulses_per_rev);
    portENTER_CRITICAL(&fan->lock);
    fan->rpm = rpm;
    fan->window_start_ms = now_ms;
    portEXIT_CRITICAL(&fan->lock);
}

fx_fan_status_t fx_fan_status(fx_fan_t *fan, uint32_t now_ms)
{
    fx_fan_status_t st = {FX_FAN_OFF, 0u, 0u, false};
    if (!fan || !fan->ready) {
        return st;
    }
    portENTER_CRITICAL(&fan->lock);
    st.duty_pct = fan->duty_pct;
    st.rpm = fan->rpm;
    const uint32_t since = now_ms - fan->command_ms;
    portEXIT_CRITICAL(&fan->lock);
    st.tach = fan->pcnt != NULL;
    st.state = fx_fan_evaluate(&fan->params, st.duty_pct, st.tach, st.rpm, since);
    return st;
}
