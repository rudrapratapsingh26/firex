/*
 * Fan health from commanded duty and tachometer speed. Pure C.
 *
 * Gas and smoke response time depends on air moving past the MQ sensors, so a
 * fan that is commanded on but not turning is a fault the record must show
 * (ledger event fan_state: on | off | failed).
 */
#ifndef FX_FAN_HEALTH_H
#define FX_FAN_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_FAN_OFF = 0,
    FX_FAN_ON = 1,
    FX_FAN_FAILED = 2
} fx_fan_state_t;

typedef struct {
    uint32_t spinup_ms;     /* 3000 - grace period after a command before judging */
    uint32_t min_rpm;       /* 300  - below this a commanded fan counts as stopped */
    uint8_t pulses_per_rev; /* 2    - standard PC fan tachometer */
} fx_fan_params_t;

void fx_fan_defaults(fx_fan_params_t *p);

/* RPM from tachometer pulses counted over window_ms. */
uint32_t fx_fan_rpm(uint32_t pulses, uint32_t window_ms, uint8_t pulses_per_rev);

/* duty_pct 0 means commanded off. Without a tachometer a commanded fan is
 * reported ON: unmeasured, which the bring-up notes say plainly. */
fx_fan_state_t fx_fan_evaluate(const fx_fan_params_t *p, uint8_t duty_pct, bool tach_present,
                               uint32_t rpm, uint32_t ms_since_command);

const char *fx_fan_state_name(fx_fan_state_t s);

#ifdef __cplusplus
}
#endif
#endif /* FX_FAN_HEALTH_H */
