#include "fx_ntc_driver.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ntc";

static int ntc_init(fx_sensor_t *s)
{
    fx_ntc_driver_t *d = (fx_ntc_driver_t *)s;
    if (!d->adc || !d->adc->dev || d->channel > 3u) {
        ESP_LOGE(TAG, "no usable ADS1115 input for the thermistor: heat stays invalid");
        return -1;
    }
    float v = 0.0f;
    if (fx_ads1115_read_volts(d->adc, d->channel, &v) != ESP_OK) {
        ESP_LOGE(TAG, "first conversion on AIN%u failed", d->channel);
        return -1;
    }
    /* An open or shorted part still starts the task, so a reseated one recovers; until
     * then every reading is invalid. */
    const fx_ntc_status_t st = fx_ntc_read(&d->ntc, v, NULL, NULL);
    if (st != FX_NTC_OK) {
        ESP_LOGW(TAG, "first reading %.3f V on AIN%u: %s", (double)v, d->channel, fx_ntc_status_name(st));
    }
    return 0;
}

static void ntc_poll(fx_sensor_t *s, uint32_t now_ms)
{
    fx_ntc_driver_t *d = (fx_ntc_driver_t *)s;
    float v = 0.0f, ohms = 0.0f, temp_c = 0.0f;
    const bool ok = fx_ads1115_read_volts(d->adc, d->channel, &v) == ESP_OK;
    const fx_ntc_status_t st = ok ? fx_ntc_read(&d->ntc, v, &ohms, &temp_c) : FX_NTC_OK;

    portENTER_CRITICAL(&d->lock);
    d->io_error = !ok;
    if (!ok) {
        d->reads_failed++;
    } else {
        d->last_v = v;
        d->last_ohms = (st == FX_NTC_OPEN || st == FX_NTC_SHORT) ? 0.0f : ohms;
        d->last_status = st;
        d->out_of_range = st != FX_NTC_OK;
        if (st == FX_NTC_OK) {
            d->reads_ok++;
            fx_heat_push(&d->heat, now_ms, temp_c, 0.0f); /* humidity comes from the DHT22 */
        } else {
            d->reads_out_of_range++;
        }
    }
    portEXIT_CRITICAL(&d->lock);
}

static void ntc_score(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT])
{
    fx_ntc_driver_t *d = (fx_ntc_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const fx_heat_result_t r = fx_heat_evaluate(&d->heat, now_ms);
    portEXIT_CRITICAL(&d->lock);
    samples[FX_HEAT] = r.heat; /* bad readings were never pushed: staleness makes it invalid */
}

static int ntc_raw(fx_sensor_t *s, char *buf, size_t len)
{
    fx_ntc_driver_t *d = (fx_ntc_driver_t *)s;
    portENTER_CRITICAL(&d->lock);
    const bool have_v = d->reads_ok + d->reads_out_of_range > 0u;
    const bool io_error = d->io_error;
    const float v = d->last_v, ohms = d->last_ohms;
    const fx_ntc_status_t st = d->last_status;
    const fx_heat_result_t r = fx_heat_evaluate(&d->heat, d->heat.last_ms);
    const bool have_temp = d->heat.have;
    portEXIT_CRITICAL(&d->lock);

    const char *status = io_error ? "io_error" : (have_v ? fx_ntc_status_name(st) : "none");
    if (!have_v) {
        return snprintf(buf, len, ",,,,%s", status);
    }
    char ohms_text[16] = "";
    if (ohms > 0.0f) {
        snprintf(ohms_text, sizeof(ohms_text), "%.0f", (double)ohms);
    }
    /* Temperature only while the part reads good, so a capture never shows a stale
     * value next to an open or shorted status. */
    if (st != FX_NTC_OK || !have_temp) {
        return snprintf(buf, len, "%.4f,%s,,,%s", (double)v, ohms_text, status);
    }
    return snprintf(buf, len, "%.4f,%s,%.2f,%.2f,%s", (double)v, ohms_text, (double)r.temp_c,
                    r.ror_known ? (double)r.ror_c_min : 0.0, status);
}

void fx_ntc_driver_setup(fx_ntc_driver_t *d, fx_ads1115_t *adc, uint8_t channel, uint32_t period_ms,
                         const fx_ntc_params_t *ntc, const fx_heat_params_t *heat)
{
    memset(d, 0, sizeof(*d));
    portMUX_INITIALIZE(&d->lock);
    d->adc = adc;
    d->channel = channel;
    if (ntc) {
        d->ntc = *ntc;
    } else {
        fx_ntc_defaults(&d->ntc);
    }
    fx_heat_init(&d->heat, heat);

    d->sensor.name = "ntc";
    d->sensor.modalities = FX_MOD_BIT(FX_HEAT);
    d->sensor.period_ms = period_ms;
    d->sensor.raw_header = "ntc_v,ntc_ohms,temp_c,ror_c_min,ntc_status";
    d->sensor.init = ntc_init;
    d->sensor.poll = ntc_poll;
    d->sensor.score = ntc_score;
    d->sensor.raw = ntc_raw;
    d->sensor.ctx = d;
}

void fx_ntc_driver_set_params(fx_ntc_driver_t *d, const fx_heat_params_t *heat)
{
    portENTER_CRITICAL(&d->lock);
    d->heat.p = *heat;
    if (d->heat.p.min_points < 2u) {
        d->heat.p.min_points = 2u;
    }
    portEXIT_CRITICAL(&d->lock);
}
