#include "fx_selftest.h"

#include <string.h>

void fx_selftest_begin(fx_selftest_report_t *r)
{
    memset(r, 0, sizeof(*r));
}

void fx_selftest_add(fx_selftest_report_t *r, const char *name, bool ok)
{
    if (r->n < FX_SELFTEST_MAX_CHECKS) {
        r->checks[r->n].name = name;
        r->checks[r->n].ok = ok;
        r->n++;
    }
}

bool fx_selftest_pass(const fx_selftest_report_t *r)
{
    if (r->n == 0) {
        return false;
    }
    for (size_t i = 0; i < r->n; i++) {
        if (!r->checks[i].ok) {
            return false;
        }
    }
    return true;
}

bool fx_check_heater(bool sample_valid, float rs_ohms, float r0_ohms)
{
    if (!sample_valid || !(r0_ohms > 0.0f) || !(rs_ohms > 0.0f)) {
        return false;
    }
    const float ratio = rs_ohms / r0_ohms;
    return ratio >= 0.2f && ratio <= 5.0f;
}

bool fx_check_flame_loopback(float score_before, float score_during)
{
    return score_before < 0.3f && score_during >= 0.6f;
}

bool fx_check_heap(uint32_t free_bytes, uint32_t min_bytes)
{
    return free_bytes >= min_bytes;
}

bool fx_selftest_due(bool have_run, uint32_t last_run_ms, uint32_t now_ms, uint32_t period_h, uint32_t first_after_ms)
{
    if (!have_run) {
        return now_ms >= first_after_ms;
    }
    const uint64_t period_ms = (uint64_t)period_h * 3600000u;
    return (uint64_t)(uint32_t)(now_ms - last_run_ms) >= period_ms;
}
