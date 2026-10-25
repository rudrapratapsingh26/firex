#include "flame_score.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define FX_TWO_PI 6.2831853f

void fx_flame_defaults(fx_flame_params_t *p)
{
    if (!p) {
        return;
    }
    p->sample_hz = 100.0f;
    p->window_s = 2.0f;
    p->band_lo_hz = 1.0f;
    p->band_hi_hz = 15.0f;
    p->rms_quiet = 15.0f;
    p->rms_full = 80.0f;
    p->deadband = 0.5f;
    p->rail_lo = 0u;
    p->rail_hi = 4095u;
}

static float smoothing(float cutoff_hz, float sample_hz)
{
    return 1.0f - expf(-FX_TWO_PI * cutoff_hz / sample_hz);
}

void fx_flame_init(fx_flame_t *f, const fx_flame_params_t *p)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
    if (p) {
        f->p = *p;
    } else {
        fx_flame_defaults(&f->p);
    }
    f->a_hp = smoothing(f->p.band_lo_hz, f->p.sample_hz);
    f->a_lp = smoothing(f->p.band_hi_hz, f->p.sample_hz);
    float n = f->p.window_s * f->p.sample_hz;
    if (n < 8.0f) {
        n = 8.0f;
    }
    f->n = n > (float)FX_FLAME_WINDOW_MAX ? (uint16_t)FX_FLAME_WINDOW_MAX : (uint16_t)n;
}

void fx_flame_push(fx_flame_t *f, uint16_t raw)
{
    if (!f || f->n == 0u) {
        return;
    }
    const float x = (float)raw;
    if (!f->primed) {
        f->slow = x; /* start the high-pass settled, so power-on is not a flicker */
        f->fast = 0.0f;
        f->primed = true;
    }
    f->slow += f->a_hp * (x - f->slow);
    f->fast += f->a_lp * ((x - f->slow) - f->fast);

    f->band[f->head] = f->fast;
    f->raw[f->head] = raw;
    f->head = (uint16_t)((f->head + 1u) % f->n);
    if (f->count < f->n) {
        f->count++;
    }
}

fx_flame_result_t fx_flame_evaluate(const fx_flame_t *f)
{
    fx_flame_result_t r = {0.0f, 0.0f, 0.0f, true};
    if (!f || f->n == 0u) {
        r.valid = false;
        return r;
    }
    if (f->count < f->n) {
        return r; /* first window still filling: valid, nothing seen yet */
    }

    /* Oldest sample first, so the crossing count follows time order. */
    uint16_t raw_min = 0xFFFFu, raw_max = 0u;
    float mean = 0.0f;
    for (uint16_t i = 0; i < f->n; i++) {
        const uint16_t idx = (uint16_t)((f->head + i) % f->n);
        if (f->raw[idx] < raw_min) raw_min = f->raw[idx];
        if (f->raw[idx] > raw_max) raw_max = f->raw[idx];
        mean += f->band[idx];
    }
    mean /= (float)f->n;

    const bool flat = (unsigned)(raw_max - raw_min) <= 2u;
    if (flat && (raw_max <= f->p.rail_lo + 2u || raw_min + 2u >= f->p.rail_hi)) {
        r.valid = false;
        return r;
    }

    float energy = 0.0f;
    for (uint16_t i = 0; i < f->n; i++) {
        const float v = f->band[(f->head + i) % f->n] - mean;
        energy += v * v;
    }
    r.band_rms = sqrtf(energy / (float)f->n);

    const float h = f->p.deadband * r.band_rms;
    int side = 0;
    uint16_t crossings = 0u;
    for (uint16_t i = 0; i < f->n; i++) {
        const float v = f->band[(f->head + i) % f->n] - mean;
        if (v > h && side <= 0) {
            if (side < 0) crossings++;
            side = 1;
        } else if (v < -h && side >= 0) {
            if (side > 0) crossings++;
            side = -1;
        }
    }
    const float window_s = (float)f->n / f->p.sample_hz;
    r.flicker_hz = (float)crossings / (2.0f * window_s);

    if (r.flicker_hz < f->p.band_lo_hz || r.flicker_hz > f->p.band_hi_hz ||
        !(f->p.rms_full > f->p.rms_quiet)) {
        return r;
    }
    float score = (r.band_rms - f->p.rms_quiet) / (f->p.rms_full - f->p.rms_quiet);
    r.score = score < 0.0f ? 0.0f : (score > 1.0f ? 1.0f : score);
    return r;
}
