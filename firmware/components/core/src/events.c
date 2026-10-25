#include "fx_events.h"

#include <string.h>

#include "fx_cbor.h"

const char *fx_modality_key(fx_modality_t m)
{
    switch (m) {
    case FX_GAS: return "gas";
    case FX_SMOKE: return "smoke";
    case FX_FLAME: return "flame";
    case FX_HEAT: return "heat";
    case FX_HUMIDITY: return "humidity";
    default: return "unknown";
    }
}

uint32_t fx_hundredths(float value)
{
    if (!(value > 0.0f)) {
        return 0u;
    }
    if (value >= 1.0f) {
        return 100u;
    }
    return (uint32_t)(value * 100.0f + 0.5f);
}

size_t fx_ev_boot(uint8_t *out, const char *fw_version, const char *reset_reason, uint32_t boot_count)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "fw_version", fw_version);
    fx_map_text(&m, "reset_reason", reset_reason);
    fx_map_uint(&m, "boot_count", boot_count);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_self_test(uint8_t *out, bool pass, const fx_check_t *checks, size_t n)
{
    fx_map_t inner;
    uint8_t inner_buf[256];
    fx_map_init(&inner);
    for (size_t i = 0; i < n; i++) {
        fx_map_bool(&inner, checks[i].name, checks[i].ok);
    }
    const size_t inner_len = fx_map_finish(&inner, inner_buf, sizeof(inner_buf));
    if (inner_len == 0) {
        return 0;
    }
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "result", pass ? "pass" : "fail");
    fx_map_raw(&m, "checks", inner_buf, inner_len);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_threshold_cross(uint8_t *out, fx_modality_t modality, float score, bool up)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "modality", fx_modality_key(modality));
    fx_map_uint(&m, "score", fx_hundredths(score));
    fx_map_text(&m, "direction", up ? "up" : "down");
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_state_change(uint8_t *out, fx_state_t from, fx_state_t to, const fx_modality_t *reasons, size_t n_reasons,
                          const float effective[FX_MODALITY_COUNT], const bool valid[FX_MODALITY_COUNT])
{
    uint8_t reason_buf[64];
    fx_cbor_t w;
    fx_cbor_init(&w, reason_buf, sizeof(reason_buf));
    fx_cbor_array(&w, n_reasons);
    for (size_t i = 0; i < n_reasons; i++) {
        fx_cbor_text(&w, fx_modality_key(reasons[i]));
    }
    if (w.overflow) {
        return 0;
    }

    fx_map_t scores;
    uint8_t scores_buf[128];
    fx_map_init(&scores);
    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        if (valid[i]) {
            fx_map_uint(&scores, fx_modality_key((fx_modality_t)i), fx_hundredths(effective[i]));
        }
    }
    const size_t scores_len = fx_map_finish(&scores, scores_buf, sizeof(scores_buf));
    if (scores_len == 0) {
        return 0;
    }

    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "from", fx_state_name(from));
    fx_map_text(&m, "to", fx_state_name(to));
    fx_map_raw(&m, "reason", reason_buf, w.len);
    fx_map_raw(&m, "scores", scores_buf, scores_len);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_calibration(uint8_t *out, fx_modality_t modality, uint32_t r0_ohms, const char *method)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "modality", fx_modality_key(modality));
    fx_map_uint(&m, "r0", r0_ohms);
    fx_map_text(&m, "method", method);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_config_change(uint8_t *out, const char *key, uint32_t old_value, uint32_t new_value)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "key", key);
    fx_map_uint(&m, "old", old_value);
    fx_map_uint(&m, "new", new_value);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_silence(uint8_t *out, uint32_t seconds)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_uint(&m, "seconds", seconds);
    fx_map_text(&m, "by", "button");
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_fan_state(uint8_t *out, const char *state, uint32_t duty, const char *measure_key, uint32_t measure)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "state", state);
    fx_map_uint(&m, "duty", duty);
    if (measure_key) {
        fx_map_uint(&m, measure_key, measure);
    }
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_media_captured(uint8_t *out, const uint8_t frames[][32], size_t n_frames, const uint8_t *clip,
                            const char *codec, const char *resolution)
{
    uint8_t frames_buf[8 * 34 + 1];
    if (n_frames > 8u) {
        return 0;
    }
    fx_cbor_t w;
    fx_cbor_init(&w, frames_buf, sizeof(frames_buf));
    fx_cbor_array(&w, n_frames);
    for (size_t i = 0; i < n_frames; i++) {
        fx_cbor_bytes(&w, frames[i], 32);
    }
    if (w.overflow) {
        return 0;
    }
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "trigger", "CRITICAL");
    fx_map_raw(&m, "frames", frames_buf, w.len);
    if (clip) {
        fx_map_bytes(&m, "clip", clip, 32);
    } else {
        fx_map_null(&m, "clip");
    }
    fx_map_text(&m, "codec", codec);
    fx_map_text(&m, "resolution", resolution);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_detect_crossings(bool prev[FX_MODALITY_COUNT], const bool active[FX_MODALITY_COUNT],
                           const bool valid[FX_MODALITY_COUNT], const float effective[FX_MODALITY_COUNT],
                           fx_crossing_t out[FX_MODALITY_COUNT])
{
    size_t n = 0;
    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        const bool now = valid[i] && active[i];
        if (i != FX_HUMIDITY && valid[i] && now != prev[i]) {
            out[n].modality = (fx_modality_t)i;
            out[n].up = now;
            out[n].score = effective[i];
            n++;
        }
        prev[i] = now;
    }
    return n;
}

void fx_clock_init(fx_clock_t *c, bool have_last_entry, uint64_t last_entry_ts, uint64_t build_epoch_s)
{
    memset(c, 0, sizeof(*c));
    uint64_t base = build_epoch_s;
    if (have_last_entry && last_entry_ts + 1u > base) {
        base = last_entry_ts + 1u;
    }
    c->base_s = base;
    c->floor_s = have_last_entry ? last_entry_ts : 0u;
}

/* What the clock says at this uptime in milliseconds, before the floor. */
static uint64_t clock_raw_ms(const fx_clock_t *c, uint64_t uptime_ms)
{
    if (!c->synced) {
        return c->base_s * 1000u + uptime_ms;
    }
    if (uptime_ms >= c->sync_uptime_ms) {
        return c->sync_unix_ms + (uptime_ms - c->sync_uptime_ms);
    }
    /* an event queued before the sync arrived and written after it */
    const uint64_t back = c->sync_uptime_ms - uptime_ms;
    return c->sync_unix_ms > back ? c->sync_unix_ms - back : 0u;
}

uint64_t fx_clock_now(const fx_clock_t *c, uint64_t uptime_ms, fx_ts_src_t *src)
{
    if (src) {
        *src = c->synced ? FX_TS_GATEWAY : FX_TS_UNCERTAIN;
    }
    const uint64_t ts = clock_raw_ms(c, uptime_ms) / 1000u;
    return ts > c->floor_s ? ts : c->floor_s;
}

uint64_t fx_clock_stamp(fx_clock_t *c, uint64_t uptime_ms, fx_ts_src_t *src)
{
    const uint64_t ts = fx_clock_now(c, uptime_ms, src);
    c->floor_s = ts;
    return ts;
}

bool fx_clock_sync(fx_clock_t *c, uint64_t uptime_ms, uint64_t unix_ms, uint64_t *step_ms, bool *forward)
{
    uint64_t said_ms = clock_raw_ms(c, uptime_ms);
    if (said_ms < c->floor_s * 1000u) {
        said_ms = c->floor_s * 1000u;
    }
    const bool ahead = unix_ms >= said_ms;
    const uint64_t step = ahead ? unix_ms - said_ms : said_ms - unix_ms;
    const bool first = !c->synced;
    c->synced = true;
    c->sync_unix_ms = unix_ms;
    c->sync_uptime_ms = uptime_ms;
    if (step_ms) {
        *step_ms = step;
    }
    if (forward) {
        *forward = ahead;
    }
    return first || step >= FX_CLOCK_LOG_STEP_MS;
}

size_t fx_ev_clock_sync(uint8_t *out, uint64_t step_ms, bool forward)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "source", "gateway");
    fx_map_uint(&m, "step_ms", step_ms);
    fx_map_text(&m, "direction", forward ? "forward" : "back");
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_trace_captured(uint8_t *out, fx_state_t trigger, const uint8_t trace_sha256[32], uint32_t samples,
                            uint32_t period_ms, uint32_t pre_samples)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "trigger", fx_state_name(trigger));
    fx_map_bytes(&m, "trace", trace_sha256, 32);
    fx_map_uint(&m, "samples", samples);
    fx_map_uint(&m, "period_ms", period_ms);
    fx_map_uint(&m, "pre_samples", pre_samples);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_fault(uint8_t *out, const char *source, bool raised)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "source", source);
    fx_map_text(&m, "state", raised ? "raised" : "cleared");
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

size_t fx_ev_log_overwrite(uint8_t *out, uint64_t first_seq, uint64_t last_seq, bool mirrored)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_uint(&m, "first_seq", first_seq);
    fx_map_uint(&m, "last_seq", last_seq);
    fx_map_bool(&m, "mirrored", mirrored);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}
