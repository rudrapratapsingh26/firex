/*
 * MQ-2 / MQ-135 driver: one ADS1115 channel per sensor, scored by mq_score.c.
 *
 * poll() reads the ADC in the sensor's own task; score() only reads the cached
 * result, so fusion never waits on I2C. A clean-air baseline capture runs
 * inside poll() while it is armed, and its result is picked up by whoever
 * started it (the bring-up console).
 */
#ifndef FX_MQ_DRIVER_H
#define FX_MQ_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "fx_ads1115.h"
#include "fx_sensor.h"
#include "mq_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    fx_sensor_t sensor; /* first member: the driver is reachable from the API pointer */
    fx_modality_t modality;
    fx_ads1115_t *adc;
    uint8_t channel;
    fx_mq_params_t params;

    portMUX_TYPE lock;
    float v_adc;
    float r0_ohms;
    bool have;       /* at least one successful conversion */
    bool io_error;   /* the last conversion failed */
    uint32_t last_ms;
    uint32_t read_errors;

    fx_mq_baseline_t baseline;
    bool capturing;
    fx_mq_baseline_status_t baseline_status;
    float baseline_r0;
    float baseline_spread;
    bool baseline_done;
} fx_mq_driver_t;

/* name is "mq2" or "mq135"; r0_ohms may be 0 until a baseline is captured. */
void fx_mq_driver_setup(fx_mq_driver_t *d, const char *name, fx_modality_t modality,
                        fx_ads1115_t *adc, uint8_t channel, const fx_mq_params_t *params,
                        float r0_ohms);

void fx_mq_driver_set_r0(fx_mq_driver_t *d, float r0_ohms);
float fx_mq_driver_r0(fx_mq_driver_t *d);

/* Arm a clean-air capture of `samples` readings at the poll cadence. */
void fx_mq_driver_begin_baseline(fx_mq_driver_t *d, uint16_t samples, float tolerance);

/* True once when a capture has finished; fills the outcome. A READY capture has
 * already replaced R0. */
bool fx_mq_driver_take_baseline(fx_mq_driver_t *d, fx_mq_baseline_status_t *status,
                                float *r0, float *spread);

bool fx_mq_driver_capturing(fx_mq_driver_t *d);

/* Change the scoring curve while running (from settings). */
void fx_mq_driver_set_curve(fx_mq_driver_t *d, float ratio_full);

/* The newest reading, for the self-test. False if there is no fresh one. */
bool fx_mq_driver_latest(fx_mq_driver_t *d, uint32_t now_ms, float *rs_ohms, float *r0_ohms);

#ifdef __cplusplus
}
#endif
#endif /* FX_MQ_DRIVER_H */
