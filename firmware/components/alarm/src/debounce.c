#include "fx_debounce.h"

void fx_debounce_init(fx_debounce_t *d, uint32_t debounce_ms, bool pressed_now)
{
    d->stable = pressed_now;
    d->last_raw = pressed_now;
    d->changed_ms = 0u;
    d->debounce_ms = debounce_ms;
}

bool fx_debounce_press(fx_debounce_t *d, bool raw_pressed, uint32_t now_ms)
{
    if (raw_pressed != d->last_raw) {
        d->last_raw = raw_pressed;
        d->changed_ms = now_ms;
        return false;
    }
    if (raw_pressed != d->stable && (uint32_t)(now_ms - d->changed_ms) >= d->debounce_ms) {
        d->stable = raw_pressed;
        return d->stable;
    }
    return false;
}
