/*
 * Daily self-test: when it is due, and how each result is judged. Pure C; the
 * sequencing that drives hardware lives in main/selftest_task.c.
 *
 * Checks and what they can honestly show on the dev kit:
 *   heater          MQ output plausible for a warm sensor in clean air (Rs/R0 in 0.2-5).
 *                   A proxy: the dev kit has no heater-current sense.
 *   buzzer          the driver accepted a 150 ms chirp. Nothing listens for it yet.
 *   flame_loopback  an IR LED next to the KY-026 flickers at 8 Hz and the flame score rises.
 *   fan             tachometer reads above the minimum at full duty (only if a tach is wired).
 *   camera          the ESP32-CAM captured a frame and reported its size; no image leaves it.
 *   ledger          every stored entry links, hashes and verifies.
 *   heap            free heap above a floor.
 */
#ifndef FX_SELFTEST_H
#define FX_SELFTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_SELFTEST_MAX_CHECKS 8u

typedef struct {
    fx_check_t checks[FX_SELFTEST_MAX_CHECKS];
    size_t n;
} fx_selftest_report_t;

void fx_selftest_begin(fx_selftest_report_t *r);
void fx_selftest_add(fx_selftest_report_t *r, const char *name, bool ok);
bool fx_selftest_pass(const fx_selftest_report_t *r); /* every check that ran passed, and at least one ran */

bool fx_check_heater(bool sample_valid, float rs_ohms, float r0_ohms);
bool fx_check_flame_loopback(float score_before, float score_during);
bool fx_check_heap(uint32_t free_bytes, uint32_t min_bytes);

/* First run `first_after_ms` after boot, then every `period_h` hours of uptime. */
bool fx_selftest_due(bool have_run, uint32_t last_run_ms, uint32_t now_ms, uint32_t period_h, uint32_t first_after_ms);

#ifdef __cplusplus
}
#endif
#endif /* FX_SELFTEST_H */
