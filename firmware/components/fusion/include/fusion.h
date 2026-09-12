/*
 * FireX cross-sensor fusion engine.
 *
 * Pure C11, no hardware dependencies: inputs are five scores and a clock,
 * outputs are alarm states. It compiles on a laptop, runs under the Python
 * simulator, and is the same code that runs on the node (FireX software plan
 * v1.0 section 05, v2.0 workstream 1).
 *
 * The rule set:
 *   one modality  >= 0.60 held for  5 s  -> ELEVATED
 *   two modalities>= 0.60 held for 10 s  -> CRITICAL   (different sensing physics)
 *   flame         >= 0.90 held for  3 s  -> CRITICAL   (configurable, default on)
 *   leaving a state needs the score below 0.40 (hysteresis band of 0.20)
 *   humidity never votes; it only suppresses gas when steam is the likely cause
 *   an invalid modality is excluded from voting and raises the fault flag
 */
#ifndef FIREX_FUSION_H
#define FIREX_FUSION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_MODALITY_COUNT 5

/* Ordered so that the four voters come first and humidity (context only) last. */
typedef enum {
    FX_GAS = 0,      /* MQ-2: combustible gas and smoke particulates */
    FX_SMOKE = 1,    /* MQ-135: air quality / smoke, a separate sensor from gas */
    FX_FLAME = 2,    /* IR flame sensor: flicker in the 1-15 Hz band */
    FX_HEAT = 3,     /* DHT22: temperature and rate of rise */
    FX_HUMIDITY = 4  /* DHT22: context for rejecting steam, never a trigger */
} fx_modality_t;

typedef enum {
    FX_STATE_WARMUP = 0, /* sensors stabilising; no alarm is possible */
    FX_STATE_NORMAL = 1,
    FX_STATE_ELEVATED = 2,
    FX_STATE_CRITICAL = 3
} fx_state_t;

/* One sensor reading, already normalised by its driver. */
typedef struct {
    float score; /* 0.0 - 1.0 */
    bool valid;  /* false when the sensor failed self-test: excluded, not zero */
} fx_sample_t;

typedef struct {
    float enter;               /* 0.60 - score that starts a modality voting */
    float exit;                /* 0.40 - score that stops it (hysteresis)    */
    uint32_t elevated_ms;      /* 5000  - dwell before one signal is ELEVATED */
    uint32_t critical_ms;      /* 10000 - dwell before two signals are CRITICAL */
    bool flame_alone;          /* true  - flame on its own may raise CRITICAL */
    float flame_alone_score;   /* 0.90 */
    uint32_t flame_alone_ms;   /* 3000 */
    uint32_t warmup_ms;        /* 180000 - MQ heaters need minutes from cold  */
    float rh_context;          /* 0.75 - humidity score that counts as "steamy" */
    float heat_quiet;          /* 0.30 - heat score below this is "no heat rise" */
    float gas_suppress;        /* 0.50 - factor applied to gas in that case    */
} fx_config_t;

typedef struct {
    fx_state_t state;
    bool state_changed;                     /* true on the update that moved state */
    bool fault;                             /* any modality reported invalid */
    uint8_t voters;                         /* bitmask of modalities currently voting */
    uint8_t voter_count;                    /* voters that have met the CRITICAL dwell */
    fx_modality_t reason_a;                 /* first agreeing modality, or FX_MODALITY_COUNT */
    fx_modality_t reason_b;                 /* second agreeing modality, or FX_MODALITY_COUNT */
    float effective[FX_MODALITY_COUNT];     /* scores after humidity suppression */
} fx_result_t;

typedef struct {
    fx_config_t cfg;
    fx_state_t state;
    uint32_t boot_ms;
    bool active[FX_MODALITY_COUNT];   /* latched above `enter` until below `exit` */
    uint32_t since_ms[FX_MODALITY_COUNT];
} fx_engine_t;

/* Fill cfg with the starting rule set above. */
void fx_config_defaults(fx_config_t *cfg);

/* cfg may be NULL, in which case the defaults are used. */
void fx_init(fx_engine_t *e, const fx_config_t *cfg, uint32_t now_ms);

/* Feed one set of samples. Call as often as the sensors produce readings; the
 * DHT22 floors that at one reading every 2 s. */
fx_result_t fx_update(fx_engine_t *e, const fx_sample_t samples[FX_MODALITY_COUNT], uint32_t now_ms);

const char *fx_state_name(fx_state_t s);
const char *fx_modality_name(fx_modality_t m);

#ifdef __cplusplus
}
#endif
#endif /* FIREX_FUSION_H */
