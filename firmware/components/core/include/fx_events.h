/*
 * Ledger payloads for every event the node writes, built as canonical CBOR
 * (SPEC.md section 3), plus threshold-crossing detection. Pure C.
 *
 * Scores travel as integer hundredths. Modalities are named as the ledger names
 * them: gas, smoke, flame, heat, humidity.
 */
#ifndef FX_EVENTS_H
#define FX_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fusion.h"
#include "fx_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *fx_modality_key(fx_modality_t m);
uint32_t fx_hundredths(float value); /* clamped to 0-100, rounded */

typedef struct {
    const char *name;
    bool ok;
} fx_check_t;

/* Each builder writes at most FX_PAYLOAD_MAX bytes and returns the length, or 0. */
size_t fx_ev_boot(uint8_t *out, const char *fw_version, const char *reset_reason, uint32_t boot_count);
size_t fx_ev_self_test(uint8_t *out, bool pass, const fx_check_t *checks, size_t n);
size_t fx_ev_threshold_cross(uint8_t *out, fx_modality_t m, float score, bool up);
/* reasons: the agreeing modalities (none for a return to NORMAL); scores: the
 * effective scores fusion acted on, with invalid modalities left out. */
size_t fx_ev_state_change(uint8_t *out, fx_state_t from, fx_state_t to, const fx_modality_t *reasons, size_t n_reasons,
                          const float effective[FX_MODALITY_COUNT], const bool valid[FX_MODALITY_COUNT]);
size_t fx_ev_calibration(uint8_t *out, fx_modality_t m, uint32_t r0_ohms, const char *method);
size_t fx_ev_config_change(uint8_t *out, const char *key, uint32_t old_value, uint32_t new_value);
size_t fx_ev_silence(uint8_t *out, uint32_t seconds);
/* measure_key is "current_ma" or "rpm", whichever the hardware measures; NULL for neither. */
size_t fx_ev_fan_state(uint8_t *out, const char *state, uint32_t duty, const char *measure_key, uint32_t measure);
/* clip may be NULL: no clip was recorded. */
size_t fx_ev_media_captured(uint8_t *out, const uint8_t frames[][32], size_t n_frames, const uint8_t *clip,
                            const char *codec, const char *resolution);

/* A sensor went invalid (raised) or came back (cleared); source is a modality
 * key or a subsystem name such as "camera". Added in ledger SPEC v0.3. */
size_t fx_ev_fault(uint8_t *out, const char *source, bool raised);

/* Entries the ring buffer overwrote; mirrored says whether the gateway already had them. Ledger SPEC v0.4. */
size_t fx_ev_log_overwrite(uint8_t *out, uint64_t first_seq, uint64_t last_seq, bool mirrored);

/* --- threshold crossings ---------------------------------------------------- */

typedef struct {
    fx_modality_t modality;
    bool up;
    float score;
} fx_crossing_t;

/* Compare the engine's per-modality voting flags with the previous call's.
 * Humidity never votes, so it never crosses. A modality that went invalid
 * drops silently: that is a fault, not a falling score. Updates `prev`. */
size_t fx_detect_crossings(bool prev[FX_MODALITY_COUNT], const bool active[FX_MODALITY_COUNT],
                           const bool valid[FX_MODALITY_COUNT], const float effective[FX_MODALITY_COUNT],
                           fx_crossing_t out[FX_MODALITY_COUNT]);

/* Time: the gateway set the node's clock. step_ms is the size of the correction. Ledger SPEC v0.5. */
size_t fx_ev_clock_sync(uint8_t *out, uint64_t step_ms, bool forward);

/* A trace file was closed: its SHA-256, and where the trigger sits in it. Ledger SPEC v0.5. */
size_t fx_ev_trace_captured(uint8_t *out, fx_state_t trigger, const uint8_t trace_sha256[32], uint32_t samples,
                            uint32_t period_ms, uint32_t pre_samples);

/* --- time -------------------------------------------------------------------
 * The node has no RTC. Until the gateway sets the time, ts is a best estimate
 * flagged `uncertain`: it starts one second after the newest entry already on
 * flash (or at the build time for a fresh log) and advances with uptime, so it
 * never runs backwards across a reboot. Time spent powered off is not counted;
 * the flag says so.
 *
 * From M5 the gateway sends its time after every sync. The clock takes it, and
 * entries are flagged `gateway` for the rest of the boot. Whatever the source,
 * ts never runs backwards within the chain: a gateway time earlier than the last
 * ts handed out holds ts there until real time passes it. */

#define FX_CLOCK_LOG_STEP_MS 2000u /* smaller corrections are applied without a clock_sync entry */

typedef struct {
    uint64_t base_s;         /* uncertain estimate: ts at uptime 0 */
    bool synced;             /* the gateway set the time on this boot */
    uint64_t sync_unix_ms;   /* gateway time at the last sync */
    uint64_t sync_uptime_ms; /* uptime when it arrived */
    uint64_t floor_s;        /* the newest ts handed out, or the newest on flash at boot */
} fx_clock_t;

void fx_clock_init(fx_clock_t *c, bool have_last_entry, uint64_t last_entry_ts, uint64_t build_epoch_s);
/* The ts an entry written at this uptime would get, and its source. Moves nothing. */
uint64_t fx_clock_now(const fx_clock_t *c, uint64_t uptime_ms, fx_ts_src_t *src);
/* The same, for an entry being written: later calls never return less. */
uint64_t fx_clock_stamp(fx_clock_t *c, uint64_t uptime_ms, fx_ts_src_t *src);
/* Take the gateway's time, received at this uptime. step_ms and forward describe the
 * correction against what the clock would have said. True when it is to be logged as
 * clock_sync: the first sync of a boot, or a correction of FX_CLOCK_LOG_STEP_MS or more. */
bool fx_clock_sync(fx_clock_t *c, uint64_t uptime_ms, uint64_t unix_ms, uint64_t *step_ms, bool *forward);

#ifdef __cplusplus
}
#endif
#endif /* FX_EVENTS_H */
