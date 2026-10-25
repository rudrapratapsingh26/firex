/*
 * Push-button debouncing. Pure C. A level must hold for debounce_ms before it
 * counts, so contact bounce and a brushed button produce no press.
 */
#ifndef FX_DEBOUNCE_H
#define FX_DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool stable;
    bool last_raw;
    uint32_t changed_ms;
    uint32_t debounce_ms;
} fx_debounce_t;

void fx_debounce_init(fx_debounce_t *d, uint32_t debounce_ms, bool pressed_now);

/* Call every poll with the raw level. True exactly once per confirmed press. */
bool fx_debounce_press(fx_debounce_t *d, bool raw_pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* FX_DEBOUNCE_H */
