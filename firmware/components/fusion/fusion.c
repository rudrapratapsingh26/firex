#include "fusion.h"

#include <string.h>

static bool votes(fx_modality_t m)
{
    return m != FX_HUMIDITY;
}

static uint32_t dwell(uint32_t now_ms, uint32_t since_ms)
{
    return now_ms - since_ms; /* unsigned arithmetic wraps correctly */
}

void fx_config_defaults(fx_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->enter = 0.60f;
    cfg->exit = 0.40f;
    cfg->elevated_ms = 5000u;
    cfg->critical_ms = 10000u;
    cfg->flame_alone = true;
    cfg->flame_alone_score = 0.90f;
    cfg->flame_alone_ms = 3000u;
    cfg->warmup_ms = 180000u;
    cfg->rh_context = 0.75f;
    cfg->heat_quiet = 0.30f;
    cfg->gas_suppress = 0.50f;
}

void fx_init(fx_engine_t *e, const fx_config_t *cfg, uint32_t now_ms)
{
    if (!e) {
        return;
    }
    memset(e, 0, sizeof(*e));
    if (cfg) {
        e->cfg = *cfg;
    } else {
        fx_config_defaults(&e->cfg);
    }
    e->state = FX_STATE_WARMUP;
    e->boot_ms = now_ms;
}

/* Humidity is context, never a voter: high RH with a rising gas score and no
 * heat rise is steam from a bathroom or a kitchen, so the gas score is damped
 * rather than believed. */
static void apply_humidity_context(const fx_engine_t *e,
                                   const fx_sample_t samples[FX_MODALITY_COUNT],
                                   float effective[FX_MODALITY_COUNT])
{
    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        effective[i] = samples[i].valid ? samples[i].score : 0.0f;
    }

    const bool steamy = samples[FX_HUMIDITY].valid &&
                        samples[FX_HUMIDITY].score >= e->cfg.rh_context;
    const bool no_heat_rise = !samples[FX_HEAT].valid ||
                              samples[FX_HEAT].score < e->cfg.heat_quiet;

    if (steamy && no_heat_rise) {
        effective[FX_GAS] *= e->cfg.gas_suppress;
    }
}

fx_result_t fx_update(fx_engine_t *e, const fx_sample_t samples[FX_MODALITY_COUNT], uint32_t now_ms)
{
    fx_result_t r;
    memset(&r, 0, sizeof(r));
    r.reason_a = FX_MODALITY_COUNT;
    r.reason_b = FX_MODALITY_COUNT;

    if (!e || !samples) {
        return r;
    }

    apply_humidity_context(e, samples, r.effective);

    /* 1. Track which modalities are voting, with hysteresis on the way out. */
    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        if (!samples[i].valid) {
            r.fault = true;
            e->active[i] = false;
            e->since_ms[i] = 0;
            continue;
        }
        const float score = r.effective[i];
        if (e->active[i]) {
            if (score < e->cfg.exit) {
                e->active[i] = false;
                e->since_ms[i] = 0;
            }
        } else if (score >= e->cfg.enter) {
            e->active[i] = true;
            e->since_ms[i] = now_ms;
        }
        if (e->active[i] && votes((fx_modality_t)i)) {
            r.voters |= (uint8_t)(1u << i);
        }
    }

    /* 2. Warm-up blocks every alarm, and is logged so a buyer can see how long
     *    the unit was blind after power-on. */
    if (e->state == FX_STATE_WARMUP) {
        if (dwell(now_ms, e->boot_ms) < e->cfg.warmup_ms) {
            r.state = e->state;
            return r;
        }
        e->state = FX_STATE_NORMAL;
        r.state_changed = true;
    }

    /* 3. Count voters that have held long enough for each tier. */
    uint8_t elevated_votes = 0;
    uint8_t critical_votes = 0;
    fx_modality_t first = FX_MODALITY_COUNT, second = FX_MODALITY_COUNT;

    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        if (!e->active[i] || !votes((fx_modality_t)i)) {
            continue;
        }
        const uint32_t held = dwell(now_ms, e->since_ms[i]);
        if (held >= e->cfg.elevated_ms) {
            elevated_votes++;
        }
        if (held >= e->cfg.critical_ms) {
            critical_votes++;
            if (first == FX_MODALITY_COUNT) {
                first = (fx_modality_t)i;
            } else if (second == FX_MODALITY_COUNT) {
                second = (fx_modality_t)i;
            }
        }
    }
    r.voter_count = critical_votes;

    /* 4. Flame on its own may raise CRITICAL: IR flicker detection is
     *    high-specificity, and waiting for a second modality on an open flame
     *    costs the minutes the product exists to save. */
    bool flame_alone = false;
    if (e->cfg.flame_alone && e->active[FX_FLAME] && samples[FX_FLAME].valid &&
        r.effective[FX_FLAME] >= e->cfg.flame_alone_score &&
        dwell(now_ms, e->since_ms[FX_FLAME]) >= e->cfg.flame_alone_ms) {
        flame_alone = true;
    }

    /* 5. Decide the state. Two *different* modalities are required, which is
     *    why gas and smoke are separate sensors rather than two thresholds on
     *    one MQ part. */
    fx_state_t target = FX_STATE_NORMAL;
    if (critical_votes >= 2) {
        target = FX_STATE_CRITICAL;
        r.reason_a = first;
        r.reason_b = second;
    } else if (flame_alone) {
        target = FX_STATE_CRITICAL;
        r.reason_a = FX_FLAME;
    } else if (elevated_votes >= 1) {
        target = FX_STATE_ELEVATED;
        for (int i = 0; i < FX_MODALITY_COUNT; i++) {
            if (e->active[i] && votes((fx_modality_t)i) &&
                dwell(now_ms, e->since_ms[i]) >= e->cfg.elevated_ms) {
                r.reason_a = (fx_modality_t)i;
                break;
            }
        }
    }

    if (target != e->state) {
        e->state = target;
        r.state_changed = true;
    }
    r.state = e->state;
    return r;
}

const char *fx_state_name(fx_state_t s)
{
    switch (s) {
    case FX_STATE_WARMUP: return "WARMUP";
    case FX_STATE_NORMAL: return "NORMAL";
    case FX_STATE_ELEVATED: return "ELEVATED";
    case FX_STATE_CRITICAL: return "CRITICAL";
    default: return "?";
    }
}

const char *fx_modality_name(fx_modality_t m)
{
    switch (m) {
    case FX_GAS: return "gas";
    case FX_SMOKE: return "smoke/AQ";
    case FX_FLAME: return "flame";
    case FX_HEAT: return "heat";
    case FX_HUMIDITY: return "humidity";
    default: return "none";
    }
}
