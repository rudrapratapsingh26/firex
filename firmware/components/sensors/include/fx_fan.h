/*
 * Fan control: 25 kHz PWM on one GPIO (the PWM pin of a 4-wire fan, or the gate
 * of a logic-level MOSFET for a 2- or 3-wire fan) and an optional open-collector
 * tachometer counted with PCNT. Health is judged by score/fan_health.c.
 */
#ifndef FX_FAN_H
#define FX_FAN_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "fan_health.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int pwm_gpio;
    int tach_gpio; /* -1 when no tachometer is wired */
    fx_fan_params_t params;
    pcnt_unit_handle_t pcnt;
    portMUX_TYPE lock;
    uint8_t duty_pct;
    uint32_t command_ms;
    uint32_t window_start_ms;
    uint32_t rpm;
    bool ready;
} fx_fan_t;

typedef struct {
    fx_fan_state_t state;
    uint8_t duty_pct;
    uint32_t rpm;
    bool tach;
} fx_fan_status_t;

esp_err_t fx_fan_init(fx_fan_t *fan, int pwm_gpio, int tach_gpio, uint32_t now_ms);
esp_err_t fx_fan_set_duty(fx_fan_t *fan, uint8_t duty_pct, uint32_t now_ms);

/* Call about once a second: turns the tachometer count into RPM. */
void fx_fan_poll(fx_fan_t *fan, uint32_t now_ms);

fx_fan_status_t fx_fan_status(fx_fan_t *fan, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* FX_FAN_H */
