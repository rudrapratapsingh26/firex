/*
 * Alarm output logic. Pure C: fusion state, fault and self-test flags and the
 * silence button in; the displayed state, the buzzer and the ring colour out.
 *
 *   CRITICAL   temporal-three buzzer (ISO 8201), red ring
 *   SILENCED   buzzer muted by the physical button for a fixed window, ring stays red;
 *              if the condition is still CRITICAL when the window ends, the buzzer resumes
 *   ELEVATED   amber ring, no buzzer
 *   FAULT      yellow blink; the unit keeps alarming on the sensors that remain valid
 *   SELF_TEST  blue blink
 *   WARMUP     amber pulse: no alarm is possible yet
 *   NORMAL     dim green
 *   BOOT       dim white until fusion has produced a state
 *
 * Silenced never means disabled: the button is accepted only in CRITICAL, a
 * press during a silence does not extend it, and leaving CRITICAL clears it so
 * the next CRITICAL sounds.
 */
#ifndef FX_ALARM_H
#define FX_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_ALARM_BOOT = 0,
    FX_ALARM_WARMUP,
    FX_ALARM_NORMAL,
    FX_ALARM_ELEVATED,
    FX_ALARM_CRITICAL,
    FX_ALARM_SILENCED,
    FX_ALARM_FAULT,
    FX_ALARM_SELF_TEST
} fx_alarm_state_t;

typedef struct {
    uint8_t r, g, b;
} fx_rgb_t;

typedef struct {
    uint32_t silence_ms; /* 300000 - five minutes */
    uint8_t brightness;  /* 0-255 scale applied to every ring colour; 96 by default */
} fx_alarm_config_t;

typedef struct {
    bool have_fusion;     /* false until the fusion task has produced a state */
    fx_state_t fusion;
    bool fault;
    bool self_test;
    bool silence_pressed; /* a debounced press edge */
    uint32_t now_ms;
} fx_alarm_input_t;

typedef struct {
    fx_alarm_state_t state;
    bool buzzer;
    fx_rgb_t ring;
    bool silence_started;  /* log a `silence` event */
    bool silence_ended;    /* the window ran out while still CRITICAL */
    bool silence_rejected; /* pressed outside CRITICAL */
} fx_alarm_output_t;

typedef struct {
    fx_alarm_config_t cfg;
    fx_alarm_state_t state;
    uint32_t state_since_ms;
    bool silenced;
    uint32_t silence_start_ms;
} fx_alarm_t;

void fx_alarm_defaults(fx_alarm_config_t *cfg);
void fx_alarm_init(fx_alarm_t *a, const fx_alarm_config_t *cfg, uint32_t now_ms);
fx_alarm_output_t fx_alarm_step(fx_alarm_t *a, const fx_alarm_input_t *in);

/* ISO 8201 temporal-three: 0.5 s on, 0.5 s off, three times, then 1.5 s off. */
bool fx_t3_tone(uint32_t ms_since_start);

const char *fx_alarm_state_name(fx_alarm_state_t s);

#ifdef __cplusplus
}
#endif
#endif /* FX_ALARM_H */
