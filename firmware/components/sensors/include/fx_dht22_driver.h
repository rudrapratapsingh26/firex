/*
 * DHT22 driver: the single-wire line captured with the RMT receiver, decoded by
 * dht22_frame.c, scored by heat_score.c.
 *
 * The RMT peripheral timestamps every edge in hardware, so a read neither
 * busy-waits nor disables interrupts: the only blocking part is the 2 ms start
 * pulse, spent in vTaskDelay. The line needs a 10 k pull-up to 3.3 V.
 */
#ifndef FX_DHT22_DRIVER_H
#define FX_DHT22_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "dht22_frame.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "fx_sensor.h"
#include "heat_score.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_DHT22_SYMBOLS 64u

typedef struct {
    fx_sensor_t sensor; /* first member */
    int gpio;
    rmt_channel_handle_t rx;
    QueueHandle_t done;
    rmt_symbol_word_t symbols[FX_DHT22_SYMBOLS];

    portMUX_TYPE lock;
    fx_heat_t heat;
    fx_dht22_status_t last_status;
    bool have_attempt;
    uint32_t last_attempt_ms;
    uint32_t reads_ok;
    uint32_t reads_failed;
} fx_dht22_driver_t;

void fx_dht22_driver_setup(fx_dht22_driver_t *d, int gpio, const fx_heat_params_t *params);

/* Change the heat scoring parameters while running (from settings). */
void fx_dht22_driver_set_params(fx_dht22_driver_t *d, const fx_heat_params_t *params);

/* EVT board (M6): call after fx_dht22_driver_setup when a thermistor owns heat. The
 * driver then reports humidity only, and its raw columns are dht_temp_c, rh_pct and
 * dht_status, so a capture has one temp_c column, the thermistor's. */
void fx_dht22_driver_humidity_only(fx_dht22_driver_t *d);

#ifdef __cplusplus
}
#endif
#endif /* FX_DHT22_DRIVER_H */
