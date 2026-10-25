/*
 * Flame scoring from an IR photodiode (KY-026 analog output). Pure C.
 *
 * Flames flicker in roughly the 1-15 Hz band; sunlight and incandescent or
 * halogen lamps are steady at that timescale. The score therefore comes from
 * band-limited variation, not from raw IR intensity:
 *
 *   raw -> high-pass (band_lo) -> low-pass (band_hi) -> window RMS and a
 *   hysteresis zero-crossing count -> dominant flicker frequency
 *
 * A window whose frequency falls outside the band scores 0. A window pinned to
 * an ADC rail with no variation is invalid: disconnected, shorted, or blinded
 * by direct sun, and in every one of those cases it must not vote.
 */
#ifndef FX_FLAME_SCORE_H
#define FX_FLAME_SCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_FLAME_WINDOW_MAX 256u

typedef struct {
    float sample_hz;  /* 100 - must be above 2 x band_hi */
    float window_s;   /* 2.0 */
    float band_lo_hz; /* 1.0 */
    float band_hi_hz; /* 15.0 */
    float rms_quiet;  /* 15 counts - band RMS that scores 0 */
    float rms_full;   /* 80 counts - band RMS that scores 1 */
    float deadband;   /* 0.5 - crossing hysteresis as a fraction of the RMS */
    uint16_t rail_lo; /* 0 */
    uint16_t rail_hi; /* 4095 - 12-bit ADC */
} fx_flame_params_t;

typedef struct {
    fx_flame_params_t p;
    float a_hp;
    float a_lp;
    float slow;
    float fast;
    float band[FX_FLAME_WINDOW_MAX];
    uint16_t raw[FX_FLAME_WINDOW_MAX];
    uint16_t n;     /* samples per window */
    uint16_t head;  /* next write position */
    uint16_t count; /* samples held, up to n */
    bool primed;    /* filter state initialised from the first sample */
} fx_flame_t;

typedef struct {
    float band_rms;
    float flicker_hz;
    float score;
    bool valid;
} fx_flame_result_t;

void fx_flame_defaults(fx_flame_params_t *p);
void fx_flame_init(fx_flame_t *f, const fx_flame_params_t *p);
void fx_flame_push(fx_flame_t *f, uint16_t raw);
fx_flame_result_t fx_flame_evaluate(const fx_flame_t *f);

static inline fx_sample_t fx_flame_sample(const fx_flame_result_t *r)
{
    fx_sample_t s = {r->valid ? r->score : 0.0f, r->valid};
    return s;
}

#ifdef __cplusplus
}
#endif
#endif /* FX_FLAME_SCORE_H */
