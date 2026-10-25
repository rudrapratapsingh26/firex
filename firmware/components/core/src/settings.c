#include "fx_settings.h"

#include <stddef.h>
#include <string.h>

#include "dht22_frame.h"

#ifdef FX_CAM_FRAMES_LOCKED
#define CAM_FRAMES_DEF FX_CAM_FRAMES_LOCKED
#define CAM_FRAMES_MIN FX_CAM_FRAMES_LOCKED
#define CAM_FRAMES_MAX FX_CAM_FRAMES_LOCKED
#define CAM_FRAMES_HELP "frames captured per CRITICAL; fixed in production firmware"
#else
#define CAM_FRAMES_DEF 3
#define CAM_FRAMES_MIN 0
#define CAM_FRAMES_MAX 5
#define CAM_FRAMES_HELP "frames captured per CRITICAL; 0 disables the camera"
#endif

const fx_setting_def_t FX_SETTING_DEFS[FX_SETTINGS_COUNT] = {
    [FX_S_ENTER] = {"fx_enter", 60, 1, 100, "hundredths", "score at which a modality starts voting"},
    [FX_S_EXIT] = {"fx_exit", 40, 0, 99, "hundredths", "score below which it stops voting"},
    [FX_S_ELEVATED_MS] = {"fx_elev_ms", 5000, 1000, 600000, "ms", "dwell before one modality is ELEVATED"},
    [FX_S_CRITICAL_MS] = {"fx_crit_ms", 10000, 1000, 600000, "ms", "dwell before two modalities are CRITICAL"},
    [FX_S_FLAME_ALONE] = {"fx_flame_alone", 1, 0, 1, "bool", "flame on its own may raise CRITICAL"},
    [FX_S_FLAME_SCORE] = {"fx_flame_score", 90, 1, 100, "hundredths", "flame score for CRITICAL alone"},
    [FX_S_FLAME_MS] = {"fx_flame_ms", 3000, 1000, 600000, "ms", "flame dwell for CRITICAL alone"},
    [FX_S_WARMUP_S] = {"fx_warmup_s", 180, 0, 3600, "s", "no alarm possible for this long after boot"},
    [FX_S_WARM_S] = {"fx_warm_s", 30, 0, 3600, "s", "warm-up after a reset that kept the heaters powered"},
    [FX_S_RH_CONTEXT] = {"fx_rh_ctx", 75, 0, 100, "hundredths", "humidity score that counts as steam"},
    [FX_S_HEAT_QUIET] = {"fx_heat_quiet", 30, 0, 100, "hundredths", "heat score below this is no heat rise"},
    [FX_S_GAS_SUPPRESS] = {"fx_gas_supp", 50, 0, 100, "hundredths", "factor applied to gas in steam"},
    [FX_S_MOS_PAIR] = {"fx_mos_pair", 1, 0, 1, "bool", "gas + smoke alone (both metal-oxide) may raise CRITICAL"},
    [FX_S_MQ2_R0] = {"mq2_r0", 0, 0, 10000000, "ohm", "MQ-2 clean-air resistance; 0 = not calibrated"},
    [FX_S_MQ135_R0] = {"mq135_r0", 0, 0, 10000000, "ohm", "MQ-135 clean-air resistance; 0 = not calibrated"},
    [FX_S_MQ2_FULL] = {"mq2_full", 10, 1, 99, "hundredths", "MQ-2 Rs/R0 that scores 1.0"},
    [FX_S_MQ135_FULL] = {"mq135_full", 25, 1, 99, "hundredths", "MQ-135 Rs/R0 that scores 1.0"},
    [FX_S_HEAT_START_C] = {"heat_start_c", 45, 0, 100, "C", "temperature where the absolute heat term leaves 0"},
    [FX_S_HEAT_FULL_C] = {"heat_full_c", 57, 1, 120, "C", "temperature where it reaches 1"},
    [FX_S_ROR_START] = {"ror_start", 3, 0, 60, "C/min", "rate of rise where the heat term leaves 0"},
    [FX_S_ROR_FULL] = {"ror_full", 10, 1, 60, "C/min", "rate of rise where it reaches 1"},
    [FX_S_FLAME_QUIET] = {"flame_quiet", 15, 0, 4095, "counts", "flicker RMS that scores 0"},
    [FX_S_FLAME_FULL] = {"flame_full", 80, 1, 4095, "counts", "flicker RMS that scores 1"},
    [FX_S_SILENCE_S] = {"silence_s", 300, 60, 900, "s", "silence window after a button press"},
    [FX_S_FAN_DUTY] = {"fan_duty", 100, 0, 100, "%", "fan duty while running"},
    [FX_S_CAM_FRAMES] = {"cam_frames", CAM_FRAMES_DEF, CAM_FRAMES_MIN, CAM_FRAMES_MAX, "frames", CAM_FRAMES_HELP},
    [FX_S_SELFTEST_H] = {"selftest_h", 24, 1, 168, "h", "hours between automatic self-tests"},
    [FX_S_RING_BRIGHT] = {"ring_bright", 96, 8, 255, "0-255", "status ring brightness"},
};

void fx_settings_defaults(fx_settings_t *s)
{
    for (int i = 0; i < FX_SETTINGS_COUNT; i++) {
        s->v[i] = FX_SETTING_DEFS[i].def;
    }
}

bool fx_settings_locked(int id)
{
#ifdef FX_CAM_FRAMES_LOCKED
    return id == (int)FX_S_CAM_FRAMES;
#else
    (void)id;
    return false;
#endif
}

int fx_settings_find(const char *key)
{
    if (!key) {
        return -1;
    }
    for (int i = 0; i < FX_SETTINGS_COUNT; i++) {
        if (strcmp(FX_SETTING_DEFS[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

void fx_settings_fusion(const fx_settings_t *s, fx_config_t *cfg)
{
    fx_config_defaults(cfg);
    cfg->enter = (float)s->v[FX_S_ENTER] / 100.0f;
    cfg->exit = (float)s->v[FX_S_EXIT] / 100.0f;
    cfg->elevated_ms = s->v[FX_S_ELEVATED_MS];
    cfg->critical_ms = s->v[FX_S_CRITICAL_MS];
    cfg->flame_alone = s->v[FX_S_FLAME_ALONE] != 0u;
    cfg->flame_alone_score = (float)s->v[FX_S_FLAME_SCORE] / 100.0f;
    cfg->flame_alone_ms = s->v[FX_S_FLAME_MS];
    cfg->warmup_ms = s->v[FX_S_WARMUP_S] * 1000u;
    cfg->rh_context = (float)s->v[FX_S_RH_CONTEXT] / 100.0f;
    cfg->heat_quiet = (float)s->v[FX_S_HEAT_QUIET] / 100.0f;
    cfg->gas_suppress = (float)s->v[FX_S_GAS_SUPPRESS] / 100.0f;
    cfg->mos_pair = s->v[FX_S_MOS_PAIR] != 0u;
}

void fx_settings_heat(const fx_settings_t *s, fx_heat_params_t *p)
{
    fx_heat_defaults(p);
    p->temp_start_c = (float)s->v[FX_S_HEAT_START_C];
    p->temp_full_c = (float)s->v[FX_S_HEAT_FULL_C];
    p->ror_start_c_min = (float)s->v[FX_S_ROR_START];
    p->ror_full_c_min = (float)s->v[FX_S_ROR_FULL];
}

void fx_settings_flame(const fx_settings_t *s, fx_flame_params_t *p)
{
    fx_flame_defaults(p);
    p->rms_quiet = (float)s->v[FX_S_FLAME_QUIET];
    p->rms_full = (float)s->v[FX_S_FLAME_FULL];
}

void fx_settings_mq(const fx_settings_t *s, fx_mq_params_t *mq2, fx_mq_params_t *mq135)
{
    if (mq2) {
        mq2->ratio_full = (float)s->v[FX_S_MQ2_FULL] / 100.0f;
    }
    if (mq135) {
        mq135->ratio_full = (float)s->v[FX_S_MQ135_FULL] / 100.0f;
    }
}

float fx_settings_r0(const fx_settings_t *s, fx_modality_t modality)
{
    if (modality == FX_GAS) {
        return (float)s->v[FX_S_MQ2_R0];
    }
    if (modality == FX_SMOKE) {
        return (float)s->v[FX_S_MQ135_R0];
    }
    return 0.0f;
}

bool fx_settings_consistent(const fx_settings_t *s, const char **why)
{
    const char *reason = NULL;
    fx_config_t cfg;
    fx_settings_fusion(s, &cfg);
    if (s->v[FX_S_EXIT] >= s->v[FX_S_ENTER]) {
        reason = "fx_exit must be below fx_enter";
    } else if (!fx_config_check(&cfg, FX_DHT22_FLOOR_MS)) {
        reason = "fusion dwells must be at least the 2 s DHT22 floor, and fx_crit_ms at least fx_elev_ms";
    } else if (s->v[FX_S_HEAT_FULL_C] <= s->v[FX_S_HEAT_START_C]) {
        reason = "heat_full_c must be above heat_start_c";
    } else if (s->v[FX_S_ROR_FULL] <= s->v[FX_S_ROR_START]) {
        reason = "ror_full must be above ror_start";
    } else if (s->v[FX_S_FLAME_FULL] <= s->v[FX_S_FLAME_QUIET]) {
        reason = "flame_full must be above flame_quiet";
    }
    if (why) {
        *why = reason;
    }
    return reason == NULL;
}

fx_set_status_t fx_settings_set(fx_settings_t *s, const char *key, uint32_t value, uint32_t *old_value,
                                const char **why)
{
    const int id = fx_settings_find(key);
    if (why) {
        *why = NULL;
    }
    if (id < 0) {
        return FX_SET_UNKNOWN;
    }
    if (fx_settings_locked(id)) {
        if (why) {
            *why = "fixed in production firmware (docs/media-policy.md)";
        }
        return FX_SET_LOCKED;
    }
    const fx_setting_def_t *d = &FX_SETTING_DEFS[id];
    if (value < d->min || value > d->max) {
        if (why) {
            *why = "value outside the allowed range";
        }
        return FX_SET_RANGE;
    }
    fx_settings_t trial = *s;
    trial.v[id] = value;
    if (!fx_settings_consistent(&trial, why)) {
        return FX_SET_INCONSISTENT;
    }
    if (old_value) {
        *old_value = s->v[id];
    }
    *s = trial;
    return FX_SET_OK;
}
