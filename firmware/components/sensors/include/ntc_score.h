/*
 * NTC thermistor readings for the EVT board's heat sensor. Pure C.
 *
 * The thermistor is the bottom of a divider: a fixed resistor from the divider supply
 * to the ADS1115 input, the thermistor from that input to GND. The resistance comes
 * from the measured voltage, and the temperature from the Beta equation referenced to
 * 25 C:
 *
 *   1/T = 1/298.15 K + ln(R / R25) / B
 *
 * A reading at either rail is an open or shorted part, and a temperature outside
 * [min_c, max_c] is a fault too: both are invalid, never a temperature and never zero.
 * Good temperatures go to heat_score.c exactly as the DHT22's do; the thermistor's small
 * thermal mass and its 500 ms cadence are what the EVT board gains.
 *
 * The Beta model is an approximation. Away from 25 C it differs from a part's own R/T
 * table by a few percent (a common 10 k / 3950 part tabulated at 32.65 k at 0 C reads
 * as about 0.6 C), which is why calibration compares the reading with a reference
 * thermometer (docs/calibration.md section 8).
 */
#ifndef FX_NTC_SCORE_H
#define FX_NTC_SCORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float fixed_ohms; /* 10000 - divider resistor from the supply to the input */
    float r25_ohms;   /* 10000 - thermistor resistance at 25 C */
    float beta;       /* 3950  - B constant, kelvin */
    float supply_v;   /* 3.3   - divider supply, measured on the board */
    float min_c;      /* -40   - colder than this is a fault, not a room */
    float max_c;      /* 150   - hotter than this is a fault, not a reading */
} fx_ntc_params_t;

typedef enum {
    FX_NTC_OK = 0,
    FX_NTC_OPEN,        /* the input sits at the supply rail: thermistor missing or open */
    FX_NTC_SHORT,       /* the input sits at GND: thermistor shorted */
    FX_NTC_OUT_OF_RANGE /* a resistance, but a temperature outside [min_c, max_c] */
} fx_ntc_status_t;

void fx_ntc_defaults(fx_ntc_params_t *p);

/* Thermistor resistance from the voltage at the ADS1115 input. False when the voltage
 * is at or beyond either rail. */
bool fx_ntc_ohms(const fx_ntc_params_t *p, float v_in, float *ohms);

/* Temperature from resistance by the Beta equation. False outside [min_c, max_c]. */
bool fx_ntc_temp_c(const fx_ntc_params_t *p, float ohms, float *temp_c);

/* Both steps with the reason for a failure. `ohms` is written whenever the voltage was
 * off the rails, `temp_c` only on FX_NTC_OK. */
fx_ntc_status_t fx_ntc_read(const fx_ntc_params_t *p, float v_in, float *ohms, float *temp_c);

const char *fx_ntc_status_name(fx_ntc_status_t s);

#ifdef __cplusplus
}
#endif
#endif /* FX_NTC_SCORE_H */
