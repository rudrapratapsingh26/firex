#include "mq_score.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define RAIL_MARGIN_V 0.02f

void fx_mq2_defaults(fx_mq_params_t *p)
{
    if (!p) {
        return;
    }
    p->v_supply = 5.0f;
    p->r_load = 1000.0f;
    p->divider = 2.0f / 3.0f; /* 10k over 20k: 5 V at AO reads 3.33 V */
    p->ratio_full = 0.10f;    /* score 0.6 at Rs/R0 = 0.25, 0.4 at 0.40 */
}

void fx_mq135_defaults(fx_mq_params_t *p)
{
    if (!p) {
        return;
    }
    p->v_supply = 5.0f;
    p->r_load = 10000.0f;
    p->divider = 2.0f / 3.0f;
    p->ratio_full = 0.25f; /* MQ-135 moves less than the MQ-2 for smoke */
}

bool fx_mq_rs_ohms(const fx_mq_params_t *p, float v_adc, float *rs_ohms)
{
    if (!p || !rs_ohms || p->divider <= 0.0f || p->r_load <= 0.0f) {
        return false;
    }
    const float v_ao = v_adc / p->divider;
    if (!(v_ao > RAIL_MARGIN_V) || !(v_ao < p->v_supply - RAIL_MARGIN_V)) {
        return false;
    }
    *rs_ohms = p->r_load * (p->v_supply - v_ao) / v_ao;
    return true;
}

float fx_mq_score(const fx_mq_params_t *p, float rs_ohms, float r0_ohms)
{
    if (!p || !(r0_ohms > 0.0f) || !(rs_ohms > 0.0f) ||
        !(p->ratio_full > 0.0f) || !(p->ratio_full < 1.0f)) {
        return 0.0f;
    }
    const float ratio = rs_ohms / r0_ohms;
    if (ratio >= 1.0f) {
        return 0.0f;
    }
    if (ratio <= p->ratio_full) {
        return 1.0f;
    }
    return logf(ratio) / logf(p->ratio_full);
}

fx_sample_t fx_mq_sample(const fx_mq_params_t *p, float v_adc, float r0_ohms,
                         float *rs_out, float *ratio_out)
{
    fx_sample_t s = {0.0f, false};
    float rs = 0.0f;
    if (rs_out) {
        *rs_out = 0.0f;
    }
    if (ratio_out) {
        *ratio_out = 0.0f;
    }
    if (!fx_mq_rs_ohms(p, v_adc, &rs)) {
        return s;
    }
    if (rs_out) {
        *rs_out = rs;
    }
    if (!(r0_ohms > 0.0f)) {
        return s; /* no clean-air baseline yet: a score would be a guess */
    }
    if (ratio_out) {
        *ratio_out = rs / r0_ohms;
    }
    s.score = fx_mq_score(p, rs, r0_ohms);
    s.valid = true;
    return s;
}

void fx_mq_baseline_begin(fx_mq_baseline_t *b, uint16_t samples, float tolerance)
{
    if (!b) {
        return;
    }
    memset(b, 0, sizeof(*b));
    if (samples < 10u) {
        samples = 10u;
    }
    b->want = samples > FX_MQ_BASELINE_MAX ? (uint16_t)FX_MQ_BASELINE_MAX : samples;
    b->tolerance = tolerance;
}

static void sort_floats(float *v, uint16_t n)
{
    for (uint16_t i = 1; i < n; i++) {
        const float key = v[i];
        int j = (int)i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

fx_mq_baseline_status_t fx_mq_baseline_add(fx_mq_baseline_t *b, float rs_ohms,
                                           float *r0_out, float *spread_out)
{
    if (!b || b->want == 0u) {
        return FX_MQ_BASELINE_UNSTABLE;
    }
    if (b->count < b->want) {
        b->rs[b->count++] = rs_ohms;
    }
    if (b->count < b->want) {
        return FX_MQ_BASELINE_COLLECTING;
    }

    float sorted[FX_MQ_BASELINE_MAX];
    memcpy(sorted, b->rs, sizeof(float) * b->count);
    sort_floats(sorted, b->count);

    const uint16_t n = b->count;
    const float median = (n % 2u) ? sorted[n / 2u] : 0.5f * (sorted[n / 2u - 1u] + sorted[n / 2u]);
    const uint16_t tail = n / 10u;
    const float spread = median > 0.0f ? (sorted[n - 1u - tail] - sorted[tail]) / median : 1.0f;

    if (r0_out) {
        *r0_out = median;
    }
    if (spread_out) {
        *spread_out = spread;
    }
    return (median > 0.0f && spread <= b->tolerance) ? FX_MQ_BASELINE_READY : FX_MQ_BASELINE_UNSTABLE;
}

float fx_mq_baseline_track(float r0_ohms, float rs_ohms, float alpha, float band)
{
    if (!(r0_ohms > 0.0f) || !(rs_ohms > 0.0f)) {
        return r0_ohms;
    }
    const float deviation = fabsf(rs_ohms / r0_ohms - 1.0f);
    if (deviation > band) {
        return r0_ohms;
    }
    return r0_ohms + alpha * (rs_ohms - r0_ohms);
}
