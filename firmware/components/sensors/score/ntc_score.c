#include "ntc_score.h"

#include <math.h>
#include <stddef.h>

/* Within this of a rail the divider says only "open" or "short". At the default 10 k /
 * 10 k divider the rated range ends well inside it: -40 C is 3.22 V, 150 C is 65 mV. */
#define RAIL_MARGIN_V 0.01f
#define KELVIN_AT_0C 273.15
#define KELVIN_AT_25C 298.15

void fx_ntc_defaults(fx_ntc_params_t *p)
{
    if (!p) {
        return;
    }
    p->fixed_ohms = 10000.0f;
    p->r25_ohms = 10000.0f;
    p->beta = 3950.0f;
    p->supply_v = 3.3f;
    p->min_c = -40.0f;
    p->max_c = 150.0f;
}

bool fx_ntc_ohms(const fx_ntc_params_t *p, float v_in, float *ohms)
{
    if (!p || !ohms || !(p->fixed_ohms > 0.0f) || !(p->supply_v > 2.0f * RAIL_MARGIN_V)) {
        return false;
    }
    if (!(v_in > RAIL_MARGIN_V) || !(v_in < p->supply_v - RAIL_MARGIN_V)) {
        return false;
    }
    *ohms = p->fixed_ohms * v_in / (p->supply_v - v_in);
    return true;
}

bool fx_ntc_temp_c(const fx_ntc_params_t *p, float ohms, float *temp_c)
{
    if (!p || !temp_c || !(ohms > 0.0f) || !(p->r25_ohms > 0.0f) || !(p->beta > 0.0f)) {
        return false;
    }
    const double inv_t = 1.0 / KELVIN_AT_25C + log((double)ohms / (double)p->r25_ohms) / (double)p->beta;
    if (!(inv_t > 0.0)) {
        return false;
    }
    const double c = 1.0 / inv_t - KELVIN_AT_0C;
    if (!(c >= (double)p->min_c) || !(c <= (double)p->max_c)) {
        return false;
    }
    *temp_c = (float)c;
    return true;
}

fx_ntc_status_t fx_ntc_read(const fx_ntc_params_t *p, float v_in, float *ohms, float *temp_c)
{
    float r = 0.0f;
    if (!fx_ntc_ohms(p, v_in, &r)) {
        /* NaN, a missing parameter set and anything at or below the GND margin read as a short. */
        return (p && v_in >= p->supply_v - RAIL_MARGIN_V) ? FX_NTC_OPEN : FX_NTC_SHORT;
    }
    if (ohms) {
        *ohms = r;
    }
    float t = 0.0f;
    if (!fx_ntc_temp_c(p, r, &t)) {
        return FX_NTC_OUT_OF_RANGE;
    }
    if (temp_c) {
        *temp_c = t;
    }
    return FX_NTC_OK;
}

const char *fx_ntc_status_name(fx_ntc_status_t s)
{
    switch (s) {
    case FX_NTC_OK: return "ok";
    case FX_NTC_OPEN: return "open";
    case FX_NTC_SHORT: return "short";
    case FX_NTC_OUT_OF_RANGE: return "out_of_range";
    default: return "?";
    }
}
