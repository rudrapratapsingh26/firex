/*
 * trace_golden — writes test/trace_golden.fxt through the real trace component,
 * so the gateway (TypeScript) and tools/pilot (Python) can check their readers
 * against bytes the node would produce.
 *
 *   make trace-golden
 *
 * The episode, one sample every 500 ms from uptime 1000 ms, boot 7, with a
 * 4-sample pre-window and 3 NORMAL samples ending the episode:
 *   0-5   NORMAL    gas 50, smoke 40, flame 0, heat 100, humidity 450; alarm NORMAL; fan on at 100 %
 *   6     ELEVATED  gas 650 (the trigger, uptime 4000 ms); alarm ELEVATED
 *   7     CRITICAL  gas 700, heat 620; alarm CRITICAL
 *   8     CRITICAL  gas 720, heat 640, humidity invalid; alarm CRITICAL
 *   9-11  NORMAL    as 0-5
 * The file holds samples 2-11: pre_samples 4, samples 10, 192 bytes.
 */
#include "fx_trace.h"

#include <stdio.h>
#include <string.h>

static bool file_begin(void *ctx, const fx_trace_header_t *h)
{
    uint8_t buf[FX_TRACE_HEADER_LEN];
    fx_trace_header_encode(h, buf);
    return fwrite(buf, 1, sizeof(buf), (FILE *)ctx) == sizeof(buf);
}

static bool file_write(void *ctx, const uint8_t sample[FX_TRACE_SAMPLE_LEN])
{
    return fwrite(sample, 1, FX_TRACE_SAMPLE_LEN, (FILE *)ctx) == FX_TRACE_SAMPLE_LEN;
}

static void file_end(void *ctx, const fx_trace_header_t *h)
{
    uint8_t buf[FX_TRACE_HEADER_LEN];
    fx_trace_header_encode(h, buf);
    fseek((FILE *)ctx, 0, SEEK_SET);
    fwrite(buf, 1, sizeof(buf), (FILE *)ctx);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: trace_golden <out.fxt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    const fx_trace_writer_t w = {f, file_begin, file_write, file_end, NULL};
    fx_trace_sample_t ring[4];
    fx_trace_t t;
    const fx_trace_config_t cfg = {500, 3, 100};
    fx_trace_init(&t, &cfg, ring, 4, 7);

    int ended = 0;
    for (uint32_t i = 0; i < 12; i++) {
        fx_trace_sample_t s;
        memset(&s, 0, sizeof(s));
        const uint16_t normal[FX_MODALITY_COUNT] = {50, 40, 0, 100, 450};
        memcpy(s.score, normal, sizeof(normal));
        s.state = FX_STATE_NORMAL;
        s.alarm = 2; /* FX_ALARM_NORMAL */
        s.fan_duty = 100;
        s.fan_state = FX_TRACE_FAN_ON;
        if (i == 6) {
            s.score[FX_GAS] = 650;
            s.state = FX_STATE_ELEVATED;
            s.alarm = 3;
        } else if (i == 7) {
            s.score[FX_GAS] = 700;
            s.score[FX_HEAT] = 620;
            s.state = FX_STATE_CRITICAL;
            s.alarm = 4;
        } else if (i == 8) {
            s.score[FX_GAS] = 720;
            s.score[FX_HEAT] = 640;
            s.score[FX_HUMIDITY] = FX_TRACE_INVALID;
            s.state = FX_STATE_CRITICAL;
            s.alarm = 4;
        }
        if (fx_trace_feed(&t, &s, 1000u + i * 500u, &w) == FX_TRACE_ENDED) {
            ended = 1;
        }
    }
    fclose(f);
    if (!ended) {
        fprintf(stderr, "the golden episode did not end\n");
        return 1;
    }
    return 0;
}
