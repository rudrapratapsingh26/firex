#include "fan_health.h"

#include <stddef.h>

void fx_fan_defaults(fx_fan_params_t *p)
{
    if (!p) {
        return;
    }
    p->spinup_ms = 3000u;
    p->min_rpm = 300u;
    p->pulses_per_rev = 2u;
}

uint32_t fx_fan_rpm(uint32_t pulses, uint32_t window_ms, uint8_t pulses_per_rev)
{
    if (window_ms == 0u || pulses_per_rev == 0u) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)pulses * 60000u) / ((uint64_t)window_ms * pulses_per_rev));
}

fx_fan_state_t fx_fan_evaluate(const fx_fan_params_t *p, uint8_t duty_pct, bool tach_present,
                               uint32_t rpm, uint32_t ms_since_command)
{
    if (duty_pct == 0u) {
        return FX_FAN_OFF;
    }
    if (!p || !tach_present || ms_since_command < p->spinup_ms) {
        return FX_FAN_ON;
    }
    return rpm >= p->min_rpm ? FX_FAN_ON : FX_FAN_FAILED;
}

const char *fx_fan_state_name(fx_fan_state_t s)
{
    switch (s) {
    case FX_FAN_OFF: return "off";
    case FX_FAN_ON: return "on";
    case FX_FAN_FAILED: return "failed";
    default: return "?";
    }
}
