#include "fx_alarm.h"

#include <stddef.h>
#include <string.h>

static const fx_rgb_t RED = {255, 0, 0};
static const fx_rgb_t AMBER = {255, 110, 0};
static const fx_rgb_t YELLOW = {255, 200, 0};
static const fx_rgb_t BLUE = {0, 60, 255};
static const fx_rgb_t GREEN = {0, 255, 40};
static const fx_rgb_t WHITE = {255, 255, 255};
static const fx_rgb_t OFF = {0, 0, 0};

void fx_alarm_defaults(fx_alarm_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->silence_ms = 300000u;
    cfg->brightness = 96u;
}

void fx_alarm_init(fx_alarm_t *a, const fx_alarm_config_t *cfg, uint32_t now_ms)
{
    memset(a, 0, sizeof(*a));
    if (cfg) {
        a->cfg = *cfg;
    } else {
        fx_alarm_defaults(&a->cfg);
    }
    a->state = FX_ALARM_BOOT;
    a->state_since_ms = now_ms;
}

bool fx_t3_tone(uint32_t ms_since_start)
{
    const uint32_t t = ms_since_start % 4000u;
    return t < 500u || (t >= 1000u && t < 1500u) || (t >= 2000u && t < 2500u);
}

static fx_rgb_t scale(fx_rgb_t c, uint32_t numerator, uint32_t denominator)
{
    fx_rgb_t out = {(uint8_t)(c.r * numerator / denominator), (uint8_t)(c.g * numerator / denominator),
                    (uint8_t)(c.b * numerator / denominator)};
    return out;
}

fx_alarm_output_t fx_alarm_step(fx_alarm_t *a, const fx_alarm_input_t *in)
{
    fx_alarm_output_t out;
    memset(&out, 0, sizeof(out));
    const uint32_t now = in->now_ms;
    const bool critical = in->have_fusion && in->fusion == FX_STATE_CRITICAL;

    /* 1. Silence window. */
    if (a->silenced) {
        if (!critical) {
            a->silenced = false; /* the condition cleared: the next CRITICAL must sound */
        } else if (now - a->silence_start_ms >= a->cfg.silence_ms) {
            a->silenced = false;
            out.silence_ended = true;
        }
    }
    if (in->silence_pressed) {
        if (!critical) {
            out.silence_rejected = true;
        } else if (!a->silenced) {
            a->silenced = true;
            a->silence_start_ms = now;
            out.silence_started = true;
        }
    }

    /* 2. What to show, most urgent first. */
    fx_alarm_state_t target;
    if (!in->have_fusion) {
        target = FX_ALARM_BOOT;
    } else if (critical) {
        target = a->silenced ? FX_ALARM_SILENCED : FX_ALARM_CRITICAL;
    } else if (in->fusion == FX_STATE_ELEVATED) {
        target = FX_ALARM_ELEVATED;
    } else if (in->fault) {
        target = FX_ALARM_FAULT;
    } else if (in->self_test) {
        target = FX_ALARM_SELF_TEST;
    } else if (in->fusion == FX_STATE_WARMUP) {
        target = FX_ALARM_WARMUP;
    } else {
        target = FX_ALARM_NORMAL;
    }
    if (target != a->state) {
        a->state = target;
        a->state_since_ms = now; /* patterns start at their beginning: the first T3 tone is immediate */
    }
    const uint32_t phase = now - a->state_since_ms;

    /* 3. Outputs. */
    fx_rgb_t ring = OFF;
    switch (a->state) {
    case FX_ALARM_CRITICAL:
        out.buzzer = fx_t3_tone(phase);
        ring = RED;
        break;
    case FX_ALARM_SILENCED:
        ring = RED;
        break;
    case FX_ALARM_ELEVATED:
        ring = AMBER;
        break;
    case FX_ALARM_FAULT:
        ring = (phase % 1000u) < 500u ? YELLOW : OFF;
        break;
    case FX_ALARM_SELF_TEST:
        ring = (phase % 2000u) < 1000u ? BLUE : OFF;
        break;
    case FX_ALARM_WARMUP: {
        const uint32_t t = phase % 2000u;
        const uint32_t level = t < 1000u ? t : 2000u - t; /* triangle, 0-1000 */
        ring = scale(AMBER, 100u + level * 9u / 10u, 1000u);
        break;
    }
    case FX_ALARM_NORMAL:
        ring = scale(GREEN, 1u, 4u);
        break;
    case FX_ALARM_BOOT:
    default:
        ring = scale(WHITE, 1u, 8u);
        break;
    }
    out.state = a->state;
    out.ring = scale(ring, a->cfg.brightness, 255u);
    return out;
}

const char *fx_alarm_state_name(fx_alarm_state_t s)
{
    switch (s) {
    case FX_ALARM_BOOT: return "BOOT";
    case FX_ALARM_WARMUP: return "WARMUP";
    case FX_ALARM_NORMAL: return "NORMAL";
    case FX_ALARM_ELEVATED: return "ELEVATED";
    case FX_ALARM_CRITICAL: return "CRITICAL";
    case FX_ALARM_SILENCED: return "SILENCED";
    case FX_ALARM_FAULT: return "FAULT";
    case FX_ALARM_SELF_TEST: return "SELF_TEST";
    default: return "?";
    }
}
