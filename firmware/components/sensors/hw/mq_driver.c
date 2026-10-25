#include "fx_mq_driver.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

/* A reading older than this many poll periods is stale: the sensor task has
 * stopped or the bus has failed, and the score becomes invalid. */
#define STALE_PERIODS 3u

static const char *TAG = "mq";

static int mq_init(fx_sensor_t *s)
{
    fx_mq_driver_t *d = (fx_mq_driver_t *)s;
    if (!d->adc || !d->adc->dev) {
        ESP_LOGE(TAG, "%s: no ADS1115, sensor stays invalid", s->name);
        return -1;
    }
    float v = 0.0f;
    if (fx_ads1115_read_volts(d->adc, d->channel, &v) != ESP_OK) {
        ESP_LOGE(TAG, "%s: first conversion on AIN%u failed", s->name, d->channel);
        return -1;
    }
    return 0;
}

static void mq_poll(fx_sensor_t *s, uint32_t now_ms)
{
    fx_mq_driver_t *d = (fx_mq_driver_t *)s;
    float v = 0.0f;
    const bool ok = fx_ads1115_read_volts(d->adc, d->channel, &v) == ESP_OK;
    float rs = 0.0f;
    const bool have_rs = ok && fx_mq_rs_ohms(&d->params, v, &rs);

    portENTER_CRITICAL(&d->lock);
    d->io_error = !ok;
    if (ok) {
        d->v_adc = v;
        d->have = true;
        d->last_ms = now_ms;
    } else {
        d->read_errors++;
    }
    if (d->capturing && have_rs) {
        float r0 = 0.0f, spread = 0.0f;
        const fx_mq_baseline_status_t st = fx_mq_baseline_add(&d->baseline, rs, &r0, &spread);
        if (st != FX_MQ_BASELINE_COLLECTING) {
            d->capturing = false;
            d->baseline_status = st;
            d->baseline_r0 = r0;
            d->baseline_spread = spread;
            d->baseline_done = true;
            if (st == FX_MQ_BASELINE_READY) {
                d->r0_ohms = r0;
            }
        }
    }
    portEXIT_CRITICAL(&d->lock);
}

static void mq_score(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT])
{
    fx_mq_driver_t *d = (fx_mq_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool fresh = d->have && !d->io_error && (now_ms - d->last_ms) <= STALE_PERIODS * s->period_ms;
    const float v = d->v_adc;
    const float r0 = d->r0_ohms;
    portEXIT_CRITICAL(&d->lock);

    fx_sample_t out = {0.0f, false};
    if (fresh) {
        out = fx_mq_sample(&d->params, v, r0, NULL, NULL);
    }
    samples[d->modality] = out;
}

static int mq_raw(fx_sensor_t *s, char *buf, size_t len)
{
    fx_mq_driver_t *d = (fx_mq_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool have = d->have && !d->io_error;
    const float v = d->v_adc;
    const float r0 = d->r0_ohms;
    portEXIT_CRITICAL(&d->lock);

    if (!have) {
        return snprintf(buf, len, ",,");
    }
    float rs = 0.0f, ratio = 0.0f;
    (void)fx_mq_sample(&d->params, v, r0, &rs, &ratio);
    return snprintf(buf, len, "%.4f,%.0f,%.4f", (double)v, (double)rs, (double)ratio);
}

void fx_mq_driver_setup(fx_mq_driver_t *d, const char *name, fx_modality_t modality,
                        fx_ads1115_t *adc, uint8_t channel, const fx_mq_params_t *params,
                        float r0_ohms)
{
    memset(d, 0, sizeof(*d));
    portMUX_INITIALIZE(&d->lock);
    d->modality = modality;
    d->adc = adc;
    d->channel = channel;
    d->params = *params;
    d->r0_ohms = r0_ohms;

    d->sensor.name = name;
    d->sensor.modalities = FX_MOD_BIT(modality);
    d->sensor.period_ms = 1000u;
    d->sensor.raw_header = (modality == FX_GAS) ? "mq2_v,mq2_rs,mq2_ratio" : "mq135_v,mq135_rs,mq135_ratio";
    d->sensor.init = mq_init;
    d->sensor.poll = mq_poll;
    d->sensor.score = mq_score;
    d->sensor.raw = mq_raw;
    d->sensor.ctx = d;
}

void fx_mq_driver_set_r0(fx_mq_driver_t *d, float r0_ohms)
{
    portENTER_CRITICAL(&d->lock);
    d->r0_ohms = r0_ohms;
    portEXIT_CRITICAL(&d->lock);
}

float fx_mq_driver_r0(fx_mq_driver_t *d)
{
    portENTER_CRITICAL(&d->lock);
    const float r0 = d->r0_ohms;
    portEXIT_CRITICAL(&d->lock);
    return r0;
}

void fx_mq_driver_begin_baseline(fx_mq_driver_t *d, uint16_t samples, float tolerance)
{
    portENTER_CRITICAL(&d->lock);
    fx_mq_baseline_begin(&d->baseline, samples, tolerance);
    d->capturing = true;
    d->baseline_done = false;
    portEXIT_CRITICAL(&d->lock);
}

bool fx_mq_driver_take_baseline(fx_mq_driver_t *d, fx_mq_baseline_status_t *status,
                                float *r0, float *spread)
{
    portENTER_CRITICAL(&d->lock);
    const bool done = d->baseline_done;
    if (done) {
        d->baseline_done = false;
        if (status) *status = d->baseline_status;
        if (r0) *r0 = d->baseline_r0;
        if (spread) *spread = d->baseline_spread;
    }
    portEXIT_CRITICAL(&d->lock);
    return done;
}

void fx_mq_driver_set_curve(fx_mq_driver_t *d, float ratio_full)
{
    portENTER_CRITICAL(&d->lock);
    d->params.ratio_full = ratio_full;
    portEXIT_CRITICAL(&d->lock);
}

bool fx_mq_driver_latest(fx_mq_driver_t *d, uint32_t now_ms, float *rs_ohms, float *r0_ohms)
{
    portENTER_CRITICAL(&d->lock);
    const bool fresh = d->have && !d->io_error && (now_ms - d->last_ms) <= STALE_PERIODS * d->sensor.period_ms;
    const float v = d->v_adc;
    const float r0 = d->r0_ohms;
    const fx_mq_params_t params = d->params;
    portEXIT_CRITICAL(&d->lock);
    if (r0_ohms) {
        *r0_ohms = r0;
    }
    return fresh && fx_mq_rs_ohms(&params, v, rs_ohms);
}

bool fx_mq_driver_capturing(fx_mq_driver_t *d)
{
    portENTER_CRITICAL(&d->lock);
    const bool capturing = d->capturing;
    portEXIT_CRITICAL(&d->lock);
    return capturing;
}
