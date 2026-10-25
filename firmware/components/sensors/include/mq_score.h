/*
 * MQ-series gas sensor scoring: voltage -> Rs -> Rs/R0 -> hazard score. Pure C.
 *
 * The MQ-2 (gas) and MQ-135 (smoke / air quality) are both metal-oxide sensors
 * whose resistance Rs falls as the target gas rises. R0 is Rs measured in clean
 * air on this unit, so Rs/R0 is 1.0 in clean air and falls from there. The
 * score is log-linear between ratio 1.0 (score 0) and ratio_full (score 1),
 * because MQ datasheet curves are straight lines on log-log axes.
 *
 * Every constant here is a starting point to be replaced by M1
 * characterisation, then tuned in M4. None of it is calibrated.
 */
#ifndef FX_MQ_SCORE_H
#define FX_MQ_SCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float v_supply;   /* 5.0  - supply of the breakout's load circuit, volts */
    float r_load;     /* ohms - RL on the breakout; measure it, boards differ (1k-10k) */
    float divider;    /* V_adc / V_ao; the AO swing is 0-5 V and the ADS1115 runs at 3.3 V */
    float ratio_full; /* Rs/R0 at which the score reaches 1.0 */
} fx_mq_params_t;

void fx_mq2_defaults(fx_mq_params_t *p);
void fx_mq135_defaults(fx_mq_params_t *p);

/* Rs from the voltage seen at the ADC. False when the output sits at either
 * rail, which is a disconnected or shorted sensor rather than a reading. */
bool fx_mq_rs_ohms(const fx_mq_params_t *p, float v_adc, float *rs_ohms);

/* 0.0 - 1.0. Returns 0 for a non-positive r0; fx_mq_sample marks that invalid. */
float fx_mq_score(const fx_mq_params_t *p, float rs_ohms, float r0_ohms);

/* A sample for the fusion engine: invalid (never zero) with no baseline or a
 * rail reading. rs_out and ratio_out may be NULL. */
fx_sample_t fx_mq_sample(const fx_mq_params_t *p, float v_adc, float r0_ohms,
                         float *rs_out, float *ratio_out);

/* --- clean-air baseline -------------------------------------------------- */

#define FX_MQ_BASELINE_MAX 64u

typedef enum {
    FX_MQ_BASELINE_COLLECTING = 0,
    FX_MQ_BASELINE_READY = 1,
    FX_MQ_BASELINE_UNSTABLE = 2 /* the air or the heater was not settled; start again */
} fx_mq_baseline_status_t;

typedef struct {
    float rs[FX_MQ_BASELINE_MAX];
    uint16_t want;
    uint16_t count;
    float tolerance; /* allowed (p90 - p10) / median */
} fx_mq_baseline_t;

void fx_mq_baseline_begin(fx_mq_baseline_t *b, uint16_t samples, float tolerance);

/* Add one Rs taken in clean air. When enough are in, R0 is their median and the
 * spread is (p90 - p10) / median, so one spike does not fail a capture but a
 * drifting heater does. */
fx_mq_baseline_status_t fx_mq_baseline_add(fx_mq_baseline_t *b, float rs_ohms,
                                           float *r0_out, float *spread_out);

/* Slow drift tracking: move r0 toward rs by alpha, but only while rs is within
 * `band` of r0, so a gas event is never learned as the new clean air. */
float fx_mq_baseline_track(float r0_ohms, float rs_ohms, float alpha, float band);

#ifdef __cplusplus
}
#endif
#endif /* FX_MQ_SCORE_H */
