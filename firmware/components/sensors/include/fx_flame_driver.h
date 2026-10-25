/*
 * KY-026 flame driver: the module's analog output on an ESP32-S3 ADC1 pin,
 * sampled at 100 Hz and scored by flame_score.c. The on-chip ADC is fine here:
 * flicker detection needs a fast, repeatable signal, not a linear one, and the
 * ADS1115 is kept for the MQ sensors that do need linearity.
 */
#ifndef FX_FLAME_DRIVER_H
#define FX_FLAME_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_oneshot.h"
#include "flame_score.h"
#include "freertos/FreeRTOS.h"
#include "fx_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    fx_sensor_t sensor; /* first member */
    int gpio;
    adc_oneshot_unit_handle_t unit;
    adc_channel_t channel;

    fx_flame_t state;   /* touched only by the sampling task */
    uint16_t since_eval;

    portMUX_TYPE lock;
    fx_flame_result_t result; /* re-evaluated twice a second */
    uint16_t last_raw;
    bool have;
    bool io_error;
    uint32_t last_ms;
    uint32_t read_errors;
} fx_flame_driver_t;

void fx_flame_driver_setup(fx_flame_driver_t *d, int gpio, const fx_flame_params_t *params);

/* Change the flicker thresholds while running (from settings). */
void fx_flame_driver_set_thresholds(fx_flame_driver_t *d, float rms_quiet, float rms_full);

/* The latest evaluation, straight from the driver: the self-test reads this
 * while fusion is told to ignore the flame channel. */
fx_flame_result_t fx_flame_driver_result(fx_flame_driver_t *d);

#ifdef __cplusplus
}
#endif
#endif /* FX_FLAME_DRIVER_H */
