/*
 * Alarm output hardware: the buzzer and the WS2812 status ring. Both are
 * written only when their output changes, so the alarm task's 20 ms loop does
 * no work in a steady state. Neither call waits on a queue.
 */
#ifndef FX_ALARM_HW_H
#define FX_ALARM_HW_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fx_alarm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_BUZZER_ACTIVE = 0, /* has its own oscillator: the GPIO level switches it */
    FX_BUZZER_PASSIVE = 1 /* bare piezo: driven with a square wave at tone_hz */
} fx_buzzer_kind_t;

typedef struct {
    int gpio;
    fx_buzzer_kind_t kind;
    bool on;
    bool ready;
} fx_buzzer_t;

esp_err_t fx_buzzer_init(fx_buzzer_t *b, int gpio, fx_buzzer_kind_t kind, uint32_t tone_hz);
esp_err_t fx_buzzer_set(fx_buzzer_t *b, bool on);

typedef struct {
    void *strip; /* led_strip_handle_t */
    uint16_t count;
    fx_rgb_t shown;
    bool have_shown;
    bool ready;
} fx_status_ring_t;

esp_err_t fx_status_ring_init(fx_status_ring_t *r, int gpio, uint16_t count);
esp_err_t fx_status_ring_show(fx_status_ring_t *r, fx_rgb_t colour);

#ifdef __cplusplus
}
#endif
#endif /* FX_ALARM_HW_H */
