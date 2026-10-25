/*
 * NTC thermistor driver (EVT board, M6): one ADS1115 input, read in the driver's own
 * sampling task, converted by ntc_score.c and scored by heat_score.c.
 *
 * It owns the heat modality only; on the EVT board the DHT22 stays for humidity
 * (fx_dht22_driver_humidity_only). The ADS1115 is shared with both MQ drivers, whose
 * conversions its mutex already serialises. score() reads only the cached window, so
 * fusion never waits on I2C.
 */
#ifndef FX_NTC_DRIVER_H
#define FX_NTC_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "fx_ads1115.h"
#include "fx_sensor.h"
#include "heat_score.h"
#include "ntc_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    fx_sensor_t sensor; /* first member */
    fx_ads1115_t *adc;  /* NULL leaves the sensor not ready: heat stays invalid */
    uint8_t channel;
    fx_ntc_params_t ntc;

    portMUX_TYPE lock;
    fx_heat_t heat;
    float last_v;
    float last_ohms;
    fx_ntc_status_t last_status;
    bool io_error;     /* the last conversion failed */
    bool out_of_range; /* the last conversion was open, shorted or out of range */
    uint32_t reads_ok;
    uint32_t reads_failed;
    uint32_t reads_out_of_range;
} fx_ntc_driver_t;

void fx_ntc_driver_setup(fx_ntc_driver_t *d, fx_ads1115_t *adc, uint8_t channel, uint32_t period_ms,
                         const fx_ntc_params_t *ntc, const fx_heat_params_t *heat);

/* Change the heat scoring parameters while running (from settings). */
void fx_ntc_driver_set_params(fx_ntc_driver_t *d, const fx_heat_params_t *heat);

#ifdef __cplusplus
}
#endif
#endif /* FX_NTC_DRIVER_H */
