/*
 * The alarm task: highest application priority, a 20 ms loop, and nothing in
 * it waits on a queue. It reads the shared state under a spinlock, runs the
 * pure alarm logic, and drives the buzzer and the ring. With Wi-Fi, the
 * ledger and the console all dead, this loop still sounds CRITICAL.
 *
 * The one call that is not instantaneous is the ring refresh (an RMT
 * transmission of about 9 ms for 12 LEDs), made only when the colour changes
 * and always after the buzzer has been set.
 */
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "node.h"
#include "sdkconfig.h"

#define PERIOD_MS 20u
#define DEBOUNCE_MS 30u

static void alarm_task(void *arg)
{
    (void)arg;
    uint32_t version = 0;
    fx_settings_t s = node_settings_snapshot(&version);
    fx_alarm_config_t cfg;
    fx_alarm_defaults(&cfg);
    cfg.silence_ms = s.v[FX_S_SILENCE_S] * 1000u;
    cfg.brightness = (uint8_t)s.v[FX_S_RING_BRIGHT];
    fx_alarm_t alarm;
    fx_alarm_init(&alarm, &cfg, node_now_ms());

    const int button = CONFIG_FIREX_SILENCE_BUTTON_GPIO;
    if (button >= 0) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << button,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&io);
    }
    fx_debounce_t debounce;
    fx_debounce_init(&debounce, DEBOUNCE_MS, false);
    uint8_t payload[FX_PAYLOAD_MAX];

    esp_task_wdt_add(NULL);
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        esp_task_wdt_reset();
        const uint32_t now = node_now_ms();

        uint32_t v = 0;
        s = node_settings_snapshot(&v);
        if (v != version) {
            alarm.cfg.silence_ms = s.v[FX_S_SILENCE_S] * 1000u;
            alarm.cfg.brightness = (uint8_t)s.v[FX_S_RING_BRIGHT];
            version = v;
        }

        fx_alarm_input_t in = {0};
        portENTER_CRITICAL(&g_node.state_lock);
        in.have_fusion = g_node.have_fusion;
        in.fusion = g_node.fusion_state;
        in.fault = g_node.sensor_fault || g_node.ledger_fault || g_node.selftest_fault || g_node.buzzer_error;
        in.self_test = g_node.self_test_running;
        const bool chirp = now < g_node.chirp_until_ms;
        portEXIT_CRITICAL(&g_node.state_lock);

        const bool raw_pressed = button >= 0 && gpio_get_level(button) == 0;
        in.silence_pressed = fx_debounce_press(&debounce, raw_pressed, now);
        in.now_ms = now;

        const fx_alarm_output_t out = fx_alarm_step(&alarm, &in);
        const bool buzzer_ok = fx_buzzer_set(&g_node.buzzer, out.buzzer || chirp) == ESP_OK;
        fx_status_ring_show(&g_node.ring, out.ring); /* a missing ring is not a fault: the buzzer is the alarm */

        portENTER_CRITICAL(&g_node.state_lock);
        g_node.alarm_state = out.state;
        g_node.buzzer_error = !buzzer_ok;
        g_node.alarm_loops++; /* a new image's confirm check watches this advance */
        portEXIT_CRITICAL(&g_node.state_lock);

        if (out.silence_started) {
            const size_t len = fx_ev_silence(payload, alarm.cfg.silence_ms / 1000u);
            node_log("silence", payload, len);
        }

        xTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
    }
}

void node_alarm_start(void)
{
    xTaskCreate(alarm_task, "alarm", 4096, NULL, 10, NULL);
}
