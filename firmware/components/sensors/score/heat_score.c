#include "heat_score.h"

#include <stddef.h>
#include <string.h>

static float clamp01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static float ramp(float x, float start, float full)
{
    return full > start ? clamp01((x - start) / (full - start)) : 0.0f;
}

void fx_heat_defaults(fx_heat_params_t *p)
{
    if (!p) {
        return;
    }
    p->temp_start_c = 45.0f;
    p->temp_full_c = 57.0f;
    p->ror_start_c_min = 3.0f;
    p->ror_full_c_min = 10.0f;
    p->window_ms = 30000u;
    p->min_points = 5u;
    p->stale_ms = 10000u;
}

void fx_heat_init(fx_heat_t *h, const fx_heat_params_t *p)
{
    if (!h) {
        return;
    }
    memset(h, 0, sizeof(*h));
    if (p) {
        h->p = *p;
    } else {
        fx_heat_defaults(&h->p);
    }
    if (h->p.min_points < 2u) {
        h->p.min_points = 2u;
    }
}

void fx_heat_push(fx_heat_t *h, uint32_t now_ms, float temp_c, float rh_pct)
{
    if (!h) {
        return;
    }
    h->t_ms[h->head] = now_ms;
    h->temp_c[h->head] = temp_c;
    h->head = (uint8_t)((h->head + 1u) % FX_HEAT_WINDOW_MAX);
    if (h->count < FX_HEAT_WINDOW_MAX) {
        h->count++;
    }
    h->have = true;
    h->last_ms = now_ms;
    h->last_temp_c = temp_c;
    h->last_rh_pct = rh_pct;
}

fx_heat_result_t fx_heat_evaluate(const fx_heat_t *h, uint32_t now_ms)
{
    fx_heat_result_t r;
    memset(&r, 0, sizeof(r));
    if (!h || !h->have || (uint32_t)(now_ms - h->last_ms) > h->p.stale_ms) {
        return r; /* both samples invalid: a silent sensor is not a cold room */
    }
    r.temp_c = h->last_temp_c;
    r.rh_pct = h->last_rh_pct;

    /* Least-squares slope over the readings inside the window. Times are taken
     * relative to the newest reading and held in double, so a uint32 clock days
     * into uptime keeps its precision. */
    double sum_t = 0.0, sum_c = 0.0, sum_tt = 0.0, sum_tc = 0.0;
    uint32_t oldest_age = 0u;
    uint8_t used = 0u;
    for (uint8_t i = 0; i < h->count; i++) {
        const uint8_t idx = (uint8_t)((h->head + FX_HEAT_WINDOW_MAX - 1u - i) % FX_HEAT_WINDOW_MAX);
        const uint32_t age = (uint32_t)(h->last_ms - h->t_ms[idx]);
        if (age > h->p.window_ms) {
            break;
        }
        const double t = -(double)age;
        const double c = (double)h->temp_c[idx];
        sum_t += t;
        sum_c += c;
        sum_tt += t * t;
        sum_tc += t * c;
        oldest_age = age;
        used++;
    }
    if (used >= h->p.min_points && oldest_age * 2u >= h->p.window_ms) {
        const double denom = (double)used * sum_tt - sum_t * sum_t;
        if (denom > 0.0) {
            const double slope_per_ms = ((double)used * sum_tc - sum_t * sum_c) / denom;
            r.ror_c_min = (float)(slope_per_ms * 60000.0);
            r.ror_known = true;
        }
    }

    float heat = ramp(r.temp_c, h->p.temp_start_c, h->p.temp_full_c);
    if (r.ror_known) {
        const float ror = ramp(r.ror_c_min, h->p.ror_start_c_min, h->p.ror_full_c_min);
        heat = ror > heat ? ror : heat;
    }
    r.heat.score = heat;
    r.heat.valid = true;
    r.humidity.score = clamp01(r.rh_pct / 100.0f);
    r.humidity.valid = true;
    return r;
}
