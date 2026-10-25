/*
 * Heat and humidity scoring. Pure C.
 *
 * Temperature comes from the DHT22 on the dev kit, every 2 s, and from the NTC
 * thermistor on the EVT board (ntc_score.h), every 500 ms. Humidity always comes
 * from the DHT22. Each driver keeps its own fx_heat_t and reports only the
 * modalities it owns.
 *
 * Heat has two terms, the IS 2189 / ISO 7240-5 approach: absolute temperature
 * against a fixed ceiling, and rate of rise in C/min over a sliding window.
 * The score is the larger of the two. The rate of rise is a least-squares slope
 * over every reading in the window, so a missed read or a jittery cadence
 * moves it far less than a two-point difference would. The window holds up to
 * FX_HEAT_WINDOW_MAX readings: 64 fits a 30 s window at the thermistor's 500 ms.
 *
 * Humidity never votes in fusion; its score is RH / 100 so the engine's steam
 * rule (rh_context 0.75) reads as 75 % RH.
 */
#ifndef FX_HEAT_SCORE_H
#define FX_HEAT_SCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_HEAT_WINDOW_MAX 64u

typedef struct {
    float temp_start_c;    /* 45 - absolute term leaves 0 */
    float temp_full_c;     /* 57 - absolute term reaches 1 (EN 54-5 class A1 static band starts at 54) */
    float ror_start_c_min; /* 3  - rate-of-rise term leaves 0 */
    float ror_full_c_min;  /* 10 - rate-of-rise term reaches 1 (class A1R) */
    uint32_t window_ms;    /* 30000 - fifteen readings at the DHT22 floor, sixty at the thermistor's 500 ms */
    uint8_t min_points;    /* 5 */
    uint32_t stale_ms;     /* 10000 - no good reading for this long makes both invalid */
} fx_heat_params_t;

typedef struct {
    fx_heat_params_t p;
    uint32_t t_ms[FX_HEAT_WINDOW_MAX];
    float temp_c[FX_HEAT_WINDOW_MAX];
    uint8_t head;
    uint8_t count;
    bool have;
    uint32_t last_ms;
    float last_temp_c;
    float last_rh_pct;
} fx_heat_t;

typedef struct {
    float temp_c;
    float rh_pct;
    float ror_c_min;
    bool ror_known; /* false until the window holds min_points over half its span */
    fx_sample_t heat;
    fx_sample_t humidity;
} fx_heat_result_t;

void fx_heat_defaults(fx_heat_params_t *p);
void fx_heat_init(fx_heat_t *h, const fx_heat_params_t *p);

/* Record one good reading. Failed reads are simply not pushed; staleness turns
 * them into an invalid sample. The thermistor driver passes 0 for RH and never
 * reports humidity. */
void fx_heat_push(fx_heat_t *h, uint32_t now_ms, float temp_c, float rh_pct);

fx_heat_result_t fx_heat_evaluate(const fx_heat_t *h, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* FX_HEAT_SCORE_H */
