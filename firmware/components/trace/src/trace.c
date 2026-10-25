#include "fx_trace.h"

#include <string.h>

static void put_le(uint8_t *p, uint32_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get_le(const uint8_t *p, size_t n)
{
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}

uint16_t fx_trace_score(float score, bool valid)
{
    if (!valid) {
        return FX_TRACE_INVALID;
    }
    if (!(score > 0.0f)) {
        return 0u;
    }
    if (score >= 1.0f) {
        return 1000u;
    }
    return (uint16_t)(score * 1000.0f + 0.5f);
}

void fx_trace_header_encode(const fx_trace_header_t *h, uint8_t out[FX_TRACE_HEADER_LEN])
{
    memset(out, 0, FX_TRACE_HEADER_LEN);
    memcpy(out, "FXTR", 4);
    out[4] = (uint8_t)FX_TRACE_VERSION;
    out[5] = (uint8_t)FX_MODALITY_COUNT;
    put_le(out + 6, h->period_ms, 2);
    put_le(out + 8, h->pre_samples, 4);
    put_le(out + 12, h->samples, 4);
    out[16] = h->trigger;
    put_le(out + 20, h->trigger_uptime_ms, 4);
    put_le(out + 24, h->boot_count, 4);
}

bool fx_trace_header_decode(const uint8_t *in, size_t len, fx_trace_header_t *h)
{
    if (!in || len < FX_TRACE_HEADER_LEN || memcmp(in, "FXTR", 4) != 0 || in[4] != FX_TRACE_VERSION ||
        in[5] != FX_MODALITY_COUNT) {
        return false;
    }
    memset(h, 0, sizeof(*h));
    h->period_ms = (uint16_t)get_le(in + 6, 2);
    h->pre_samples = get_le(in + 8, 4);
    h->samples = get_le(in + 12, 4);
    h->trigger = in[16];
    h->trigger_uptime_ms = get_le(in + 20, 4);
    h->boot_count = get_le(in + 24, 4);
    return true;
}

void fx_trace_sample_encode(const fx_trace_sample_t *s, uint8_t out[FX_TRACE_SAMPLE_LEN])
{
    memset(out, 0, FX_TRACE_SAMPLE_LEN);
    for (int m = 0; m < FX_MODALITY_COUNT; m++) {
        put_le(out + 2 * m, s->score[m], 2);
    }
    out[10] = s->state;
    out[11] = s->alarm;
    out[12] = s->fan_duty;
    out[13] = s->fan_state;
}

void fx_trace_sample_decode(const uint8_t in[FX_TRACE_SAMPLE_LEN], fx_trace_sample_t *s)
{
    for (int m = 0; m < FX_MODALITY_COUNT; m++) {
        s->score[m] = (uint16_t)get_le(in + 2 * m, 2);
    }
    s->state = in[10];
    s->alarm = in[11];
    s->fan_duty = in[12];
    s->fan_state = in[13];
}

void fx_trace_defaults(fx_trace_config_t *cfg)
{
    cfg->period_ms = (uint16_t)FX_TRACE_PERIOD_MS;
    cfg->quiet_samples = FX_TRACE_QUIET_SAMPLES;
    cfg->max_samples = FX_TRACE_MAX_SAMPLES;
}

void fx_trace_init(fx_trace_t *t, const fx_trace_config_t *cfg, fx_trace_sample_t *ring, uint32_t ring_cap,
                   uint32_t boot_count)
{
    memset(t, 0, sizeof(*t));
    if (cfg) {
        t->cfg = *cfg;
    } else {
        fx_trace_defaults(&t->cfg);
    }
    t->ring = ring;
    t->ring_cap = ring ? ring_cap : 0u;
    t->boot_count = boot_count;
}

static bool alarming(uint8_t state)
{
    return state == FX_STATE_ELEVATED || state == FX_STATE_CRITICAL;
}

static void abandon(fx_trace_t *t, const fx_trace_writer_t *w)
{
    t->writing = false;
    if (w->abort) {
        w->abort(w->ctx);
    }
}

/* Open the episode's file and write the pre-window, oldest first. */
static fx_trace_event_t start(fx_trace_t *t, const fx_trace_sample_t *s, uint32_t uptime_ms, const fx_trace_writer_t *w)
{
    uint32_t pre = t->ring_count;
    if (t->cfg.max_samples > 0u && pre >= t->cfg.max_samples) {
        pre = t->cfg.max_samples - 1u; /* room for the trigger itself */
    }
    memset(&t->header, 0, sizeof(t->header));
    t->header.period_ms = t->cfg.period_ms;
    t->header.pre_samples = pre;
    t->header.trigger = s->state;
    t->header.trigger_uptime_ms = uptime_ms;
    t->header.boot_count = t->boot_count;
    t->in_episode = true;
    t->quiet = 0;
    t->writing = w && w->begin && w->write && w->end && w->begin(w->ctx, &t->header);
    t->header.samples = pre;
    if (!t->writing) {
        return FX_TRACE_DROPPED;
    }

    uint8_t rec[FX_TRACE_SAMPLE_LEN];
    const uint32_t oldest = (t->ring_head + t->ring_cap - t->ring_count) % (t->ring_cap ? t->ring_cap : 1u);
    const uint32_t skip = t->ring_count - pre;
    for (uint32_t i = 0; i < pre; i++) {
        fx_trace_sample_encode(&t->ring[(oldest + skip + i) % t->ring_cap], rec);
        if (!w->write(w->ctx, rec)) {
            abandon(t, w);
            return FX_TRACE_ABANDONED;
        }
    }
    return FX_TRACE_STARTED;
}

fx_trace_event_t fx_trace_feed(fx_trace_t *t, const fx_trace_sample_t *s, uint32_t uptime_ms,
                               const fx_trace_writer_t *w)
{
    if (!t || !s) {
        return FX_TRACE_IDLE;
    }
    fx_trace_event_t ev = FX_TRACE_IDLE;
    if (!t->in_episode && alarming(s->state)) {
        ev = start(t, s, uptime_ms, w);
    }

    /* The ring is always the last ring_cap samples, so a pre-window never includes its own trigger. */
    if (t->ring_cap) {
        t->ring[t->ring_head] = *s;
        t->ring_head = (t->ring_head + 1u) % t->ring_cap;
        if (t->ring_count < t->ring_cap) {
            t->ring_count++;
        }
    }

    if (!t->in_episode) {
        return ev;
    }
    if (t->writing) {
        uint8_t rec[FX_TRACE_SAMPLE_LEN];
        fx_trace_sample_encode(s, rec);
        if (!w->write(w->ctx, rec)) {
            abandon(t, w);
            ev = FX_TRACE_ABANDONED;
        } else if (ev == FX_TRACE_IDLE) {
            ev = FX_TRACE_RECORDING;
        }
    }
    t->header.samples++;
    t->quiet = s->state == FX_STATE_NORMAL ? t->quiet + 1u : 0u;
    if (t->quiet >= t->cfg.quiet_samples || t->header.samples >= t->cfg.max_samples) {
        if (t->writing) {
            w->end(w->ctx, &t->header);
            ev = FX_TRACE_ENDED;
        }
        t->in_episode = false;
        t->writing = false;
    }
    return ev;
}
