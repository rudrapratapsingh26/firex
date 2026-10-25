/*
 * Raw trace capture (M5). Pure C.
 *
 * Around every episode the node keeps the scores fusion was fed, so a pilot
 * site's alarms can be replayed later through the same engine
 * (tools/validation). The last five minutes sit in a RAM ring. When fusion
 * leaves NORMAL for ELEVATED or CRITICAL, the ring and everything after it go to
 * one file, until fusion has been NORMAL for two minutes or the file holds
 * thirty minutes. A CRITICAL inside an ELEVATED episode stays in the same file.
 *
 * File format, little-endian (docs/transport.md and ledger/SPEC.md v0.5):
 *   header, 32 bytes   "FXTR" | u8 version 1 | u8 modalities 5 | u16 period_ms |
 *                      u32 pre_samples | u32 samples | u8 trigger (fx_state_t) | 3 zero |
 *                      u32 trigger_uptime_ms | u32 boot_count | u32 zero
 *   sample, 16 bytes   u16 score[5] in thousandths, 0xFFFF invalid, in the order gas, smoke,
 *                      flame, heat, humidity (the raw scores, before humidity suppression) |
 *                      u8 fusion state | u8 alarm state | u8 fan duty % | u8 fan state | u16 zero
 * Sample i was taken at trigger_uptime_ms + (i - pre_samples) * period_ms of device uptime.
 */
#ifndef FX_TRACE_H
#define FX_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_TRACE_VERSION 1u
#define FX_TRACE_HEADER_LEN 32u
#define FX_TRACE_SAMPLE_LEN 16u
#define FX_TRACE_INVALID 0xFFFFu

#define FX_TRACE_FAN_OFF 0u
#define FX_TRACE_FAN_ON 1u
#define FX_TRACE_FAN_FAILED 2u
#define FX_TRACE_FAN_ABSENT 255u

/* The node's episode rules. */
#define FX_TRACE_PERIOD_MS 500u
#define FX_TRACE_PRE_SAMPLES 600u   /* 5 min before the trigger */
#define FX_TRACE_QUIET_SAMPLES 240u /* 2 min of NORMAL ends an episode */
#define FX_TRACE_MAX_SAMPLES 3600u  /* 30 min in one file, pre-window included */

typedef struct {
    uint16_t score[FX_MODALITY_COUNT]; /* thousandths, or FX_TRACE_INVALID */
    uint8_t state;                     /* fx_state_t */
    uint8_t alarm;                     /* fx_alarm_state_t */
    uint8_t fan_duty;                  /* % */
    uint8_t fan_state;                 /* FX_TRACE_FAN_* */
} fx_trace_sample_t;

typedef struct {
    uint16_t period_ms;
    uint32_t pre_samples;
    uint32_t samples;
    uint8_t trigger; /* fx_state_t */
    uint32_t trigger_uptime_ms;
    uint32_t boot_count;
} fx_trace_header_t;

/* 0.595 -> 595; an invalid sensor is FX_TRACE_INVALID, never zero. */
uint16_t fx_trace_score(float score, bool valid);

void fx_trace_header_encode(const fx_trace_header_t *h, uint8_t out[FX_TRACE_HEADER_LEN]);
/* False for a short buffer, a wrong magic, version or modality count. */
bool fx_trace_header_decode(const uint8_t *in, size_t len, fx_trace_header_t *h);
void fx_trace_sample_encode(const fx_trace_sample_t *s, uint8_t out[FX_TRACE_SAMPLE_LEN]);
void fx_trace_sample_decode(const uint8_t in[FX_TRACE_SAMPLE_LEN], fx_trace_sample_t *s);

/* Where an episode goes. The caller owns files and hashing. */
typedef struct {
    void *ctx;
    /* Open a file for this episode; the header's samples is 0 until end(). False drops the episode. */
    bool (*begin)(void *ctx, const fx_trace_header_t *h);
    /* One encoded sample. False abandons the file: abort() follows. */
    bool (*write)(void *ctx, const uint8_t sample[FX_TRACE_SAMPLE_LEN]);
    /* The episode is over: the final header, to be written over the placeholder. */
    void (*end)(void *ctx, const fx_trace_header_t *h);
    /* The file could not be completed and should be removed. */
    void (*abort)(void *ctx);
} fx_trace_writer_t;

typedef enum {
    FX_TRACE_IDLE = 0,  /* no episode, or one being followed without a file */
    FX_TRACE_STARTED,   /* an episode began and its file is open */
    FX_TRACE_RECORDING, /* a sample went into the open file */
    FX_TRACE_ENDED,     /* the file is complete: end() was called */
    FX_TRACE_DROPPED,   /* an episode began but begin() refused it */
    FX_TRACE_ABANDONED  /* a write failed: abort() was called */
} fx_trace_event_t;

typedef struct {
    uint16_t period_ms;
    uint32_t quiet_samples;
    uint32_t max_samples;
} fx_trace_config_t;

typedef struct {
    fx_trace_config_t cfg;
    fx_trace_sample_t *ring; /* the pre-window */
    uint32_t ring_cap;
    uint32_t ring_head; /* next slot to write */
    uint32_t ring_count;
    uint32_t boot_count;
    bool in_episode; /* following an episode, with or without a file */
    bool writing;    /* its file is open */
    uint32_t quiet;  /* consecutive NORMAL samples in the episode */
    fx_trace_header_t header;
} fx_trace_t;

void fx_trace_defaults(fx_trace_config_t *cfg);
/* ring holds ring_cap samples of pre-window (FX_TRACE_PRE_SAMPLES on the node). cfg may be NULL. */
void fx_trace_init(fx_trace_t *t, const fx_trace_config_t *cfg, fx_trace_sample_t *ring, uint32_t ring_cap,
                   uint32_t boot_count);
/* One sample, every period_ms. An episode that reaches max_samples ends; if fusion is still
 * out of NORMAL the next sample starts another, with the last ring_cap samples as its pre-window. */
fx_trace_event_t fx_trace_feed(fx_trace_t *t, const fx_trace_sample_t *s, uint32_t uptime_ms,
                               const fx_trace_writer_t *w);

#ifdef __cplusplus
}
#endif
#endif /* FX_TRACE_H */
