#include "fx_dht22_driver.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

#define START_LOW_MS 2
#define FRAME_TIMEOUT_MS 60
#define MAX_PULSES (2u * FX_DHT22_SYMBOLS)

static const char *TAG = "dht22";

static bool on_rx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user)
{
    (void)channel;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user, edata, &woken);
    return woken == pdTRUE;
}

static int dht_init(fx_sensor_t *s)
{
    fx_dht22_driver_t *d = (fx_dht22_driver_t *)s;
    d->done = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!d->done) {
        return -1;
    }
    const rmt_rx_channel_config_t cfg = {
        .gpio_num = d->gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, /* 1 tick = 1 us */
        .mem_block_symbols = FX_DHT22_SYMBOLS,
    };
    if (rmt_new_rx_channel(&cfg, &d->rx) != ESP_OK) {
        ESP_LOGE(TAG, "RMT RX channel on GPIO%d", d->gpio);
        return -1;
    }
    const rmt_rx_event_callbacks_t cbs = {.on_recv_done = on_rx_done};
    if (rmt_rx_register_event_callbacks(d->rx, &cbs, d->done) != ESP_OK || rmt_enable(d->rx) != ESP_OK) {
        ESP_LOGE(TAG, "RMT RX enable");
        return -1;
    }
    /* The RMT receiver routes the pad as an input; open-drain output on the same
     * pad lets the driver pull the line low for the start pulse. */
    gpio_set_direction(d->gpio, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(d->gpio, 1);
    return 0;
}

/* One read: start pulse, arm the receiver, release the line, wait for the frame. */
static fx_dht22_status_t read_frame(fx_dht22_driver_t *d, fx_dht22_reading_t *out)
{
    const rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 3000,   /* glitch filter, below the shortest DHT22 pulse */
        .signal_range_max_ns = 200000, /* 200 us with no edge ends the frame */
    };
    rmt_rx_done_event_data_t event;
    xQueueReset(d->done);

    gpio_set_level(d->gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(START_LOW_MS));
    if (rmt_receive(d->rx, d->symbols, sizeof(d->symbols), &rcfg) != ESP_OK) {
        gpio_set_level(d->gpio, 1);
        return FX_DHT22_SHORT_FRAME;
    }
    gpio_set_level(d->gpio, 1);

    if (xQueueReceive(d->done, &event, pdMS_TO_TICKS(FRAME_TIMEOUT_MS)) != pdTRUE) {
        return FX_DHT22_SHORT_FRAME;
    }

    fx_pulse_t pulses[MAX_PULSES];
    size_t n = 0;
    for (size_t i = 0; i < event.num_symbols && n + 2u <= MAX_PULSES; i++) {
        const rmt_symbol_word_t *sym = &event.received_symbols[i];
        const uint16_t dur[2] = {(uint16_t)sym->duration0, (uint16_t)sym->duration1};
        const uint8_t lvl[2] = {(uint8_t)sym->level0, (uint8_t)sym->level1};
        for (int k = 0; k < 2; k++) {
            if (dur[k] == 0u) {
                continue;
            }
            if (n > 0u && pulses[n - 1u].level == lvl[k]) {
                pulses[n - 1u].us = (uint16_t)(pulses[n - 1u].us + dur[k]);
            } else {
                pulses[n].level = lvl[k];
                pulses[n].us = dur[k];
                n++;
            }
        }
    }
    return fx_dht22_decode_pulses(pulses, n, out);
}

static void dht_poll(fx_sensor_t *s, uint32_t now_ms)
{
    fx_dht22_driver_t *d = (fx_dht22_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool due = fx_dht22_due(d->have_attempt, d->last_attempt_ms, now_ms);
    portEXIT_CRITICAL(&d->lock);
    if (!due) {
        return; /* the part's 2 s floor, whatever cadence the task runs at */
    }

    fx_dht22_reading_t reading;
    const fx_dht22_status_t st = read_frame(d, &reading);

    portENTER_CRITICAL(&d->lock);
    d->have_attempt = true;
    d->last_attempt_ms = now_ms;
    d->last_status = st;
    if (st == FX_DHT22_OK) {
        d->reads_ok++;
        fx_heat_push(&d->heat, now_ms, reading.temp_c, reading.rh_pct);
    } else {
        d->reads_failed++;
    }
    portEXIT_CRITICAL(&d->lock);
}

static void dht_score(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT])
{
    fx_dht22_driver_t *d = (fx_dht22_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const fx_heat_result_t r = fx_heat_evaluate(&d->heat, now_ms);
    portEXIT_CRITICAL(&d->lock);
    samples[FX_HEAT] = r.heat;
    samples[FX_HUMIDITY] = r.humidity;
}

static int dht_raw(fx_sensor_t *s, char *buf, size_t len)
{
    fx_dht22_driver_t *d = (fx_dht22_driver_t *)s;
    /* Raw columns are the last good reading regardless of age; validity of the
     * score is carried by the heat_ok and rh_ok columns. */
    portENTER_CRITICAL(&d->lock);
    const fx_heat_result_t r = fx_heat_evaluate(&d->heat, d->heat.last_ms);
    const bool have = d->heat.have;
    const fx_dht22_status_t st = d->last_status;
    portEXIT_CRITICAL(&d->lock);
    if (!have) {
        return snprintf(buf, len, ",,,%s", fx_dht22_status_name(st));
    }
    return snprintf(buf, len, "%.1f,%.1f,%.2f,%s", (double)r.temp_c, (double)r.rh_pct,
                    r.ror_known ? (double)r.ror_c_min : 0.0, fx_dht22_status_name(st));
}

static int dht_raw_humidity(fx_sensor_t *s, char *buf, size_t len)
{
    fx_dht22_driver_t *d = (fx_dht22_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool have = d->heat.have;
    const float temp_c = d->heat.last_temp_c, rh_pct = d->heat.last_rh_pct;
    const fx_dht22_status_t st = d->last_status;
    portEXIT_CRITICAL(&d->lock);
    if (!have) {
        return snprintf(buf, len, ",,%s", fx_dht22_status_name(st));
    }
    return snprintf(buf, len, "%.1f,%.1f,%s", (double)temp_c, (double)rh_pct, fx_dht22_status_name(st));
}

void fx_dht22_driver_humidity_only(fx_dht22_driver_t *d)
{
    d->sensor.modalities = FX_MOD_BIT(FX_HUMIDITY);
    d->sensor.raw_header = "dht_temp_c,rh_pct,dht_status";
    d->sensor.raw = dht_raw_humidity;
}

void fx_dht22_driver_setup(fx_dht22_driver_t *d, int gpio, const fx_heat_params_t *params)
{
    memset(d, 0, sizeof(*d));
    portMUX_INITIALIZE(&d->lock);
    d->gpio = gpio;
    fx_heat_init(&d->heat, params);

    d->sensor.name = "dht22";
    d->sensor.modalities = (uint8_t)(FX_MOD_BIT(FX_HEAT) | FX_MOD_BIT(FX_HUMIDITY));
    d->sensor.period_ms = FX_DHT22_FLOOR_MS;
    d->sensor.raw_header = "temp_c,rh_pct,ror_c_min,dht_status";
    d->sensor.init = dht_init;
    d->sensor.poll = dht_poll;
    d->sensor.score = dht_score;
    d->sensor.raw = dht_raw;
    d->sensor.ctx = d;
}

void fx_dht22_driver_set_params(fx_dht22_driver_t *d, const fx_heat_params_t *params)
{
    portENTER_CRITICAL(&d->lock);
    d->heat.p = *params;
    if (d->heat.p.min_points < 2u) {
        d->heat.p.min_points = 2u;
    }
    portEXIT_CRITICAL(&d->lock);
}
