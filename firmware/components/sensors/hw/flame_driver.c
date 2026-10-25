#include "fx_flame_driver.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#define EVAL_EVERY 50u     /* samples between evaluations: 0.5 s at 100 Hz */
#define STALE_MS 1000u     /* no sample for a second: the task or the ADC has stopped */

static const char *TAG = "flame";

static int flame_init(fx_sensor_t *s)
{
    fx_flame_driver_t *d = (fx_flame_driver_t *)s;
    adc_unit_t unit_id;
    if (adc_oneshot_io_to_channel(d->gpio, &unit_id, &d->channel) != ESP_OK || unit_id != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", d->gpio);
        return -1;
    }
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit_cfg, &d->unit) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 unit");
        return -1;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_config_channel(d->unit, d->channel, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 channel");
        return -1;
    }
    return 0;
}

static void flame_poll(fx_sensor_t *s, uint32_t now_ms)
{
    fx_flame_driver_t *d = (fx_flame_driver_t *)s;
    int raw = 0;
    const bool ok = adc_oneshot_read(d->unit, d->channel, &raw) == ESP_OK;
    if (ok) {
        fx_flame_push(&d->state, (uint16_t)(raw < 0 ? 0 : (raw > 4095 ? 4095 : raw)));
        d->since_eval++;
    }

    fx_flame_result_t result = {0.0f, 0.0f, 0.0f, true};
    const bool evaluate = ok && d->since_eval >= EVAL_EVERY;
    if (evaluate) {
        d->since_eval = 0u;
        result = fx_flame_evaluate(&d->state);
    }

    portENTER_CRITICAL(&d->lock);
    d->io_error = !ok;
    if (ok) {
        d->last_raw = (uint16_t)raw;
        d->have = true;
        d->last_ms = now_ms;
    } else {
        d->read_errors++;
    }
    if (evaluate) {
        d->result = result;
    }
    portEXIT_CRITICAL(&d->lock);
}

static void flame_score(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT])
{
    fx_flame_driver_t *d = (fx_flame_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool fresh = d->have && !d->io_error && (now_ms - d->last_ms) <= STALE_MS;
    const fx_flame_result_t r = d->result;
    portEXIT_CRITICAL(&d->lock);

    fx_sample_t out = {0.0f, false};
    if (fresh) {
        out = fx_flame_sample(&r);
    }
    samples[FX_FLAME] = out;
}

static int flame_raw(fx_sensor_t *s, char *buf, size_t len)
{
    fx_flame_driver_t *d = (fx_flame_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool have = d->have && !d->io_error;
    const uint16_t raw = d->last_raw;
    const fx_flame_result_t r = d->result;
    portEXIT_CRITICAL(&d->lock);
    if (!have) {
        return snprintf(buf, len, ",,");
    }
    return snprintf(buf, len, "%u,%.1f,%.2f", raw, (double)r.band_rms, (double)r.flicker_hz);
}

void fx_flame_driver_setup(fx_flame_driver_t *d, int gpio, const fx_flame_params_t *params)
{
    memset(d, 0, sizeof(*d));
    portMUX_INITIALIZE(&d->lock);
    d->gpio = gpio;
    fx_flame_init(&d->state, params);
    d->result.valid = true;

    d->sensor.name = "ky026";
    d->sensor.modalities = FX_MOD_BIT(FX_FLAME);
    d->sensor.period_ms = (uint32_t)(1000.0f / d->state.p.sample_hz);
    d->sensor.raw_header = "flame_raw,flame_band_rms,flame_hz";
    d->sensor.init = flame_init;
    d->sensor.poll = flame_poll;
    d->sensor.score = flame_score;
    d->sensor.raw = flame_raw;
    d->sensor.ctx = d;
}

void fx_flame_driver_set_thresholds(fx_flame_driver_t *d, float rms_quiet, float rms_full)
{
    portENTER_CRITICAL(&d->lock);
    d->state.p.rms_quiet = rms_quiet;
    d->state.p.rms_full = rms_full;
    portEXIT_CRITICAL(&d->lock);
}

fx_flame_result_t fx_flame_driver_result(fx_flame_driver_t *d)
{
    portENTER_CRITICAL(&d->lock);
    const fx_flame_result_t r = d->result;
    portEXIT_CRITICAL(&d->lock);
    return r;
}
