/* Host tests for components/trace (Unity): the file format, the pre-window, the
 * episode rules, and the committed golden file the gateway and Python tools read. */
#include "unity.h"

#include "fx_trace.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* --- a writer into memory --------------------------------------------------- */

typedef struct {
    uint8_t buf[FX_TRACE_HEADER_LEN + 64u * FX_TRACE_SAMPLE_LEN];
    size_t len;
    int begins, ends, aborts;
    bool refuse_begin;
    int fail_write_at; /* 1-based write number that fails; 0 for never */
    int writes;
} mem_t;

static bool mem_begin(void *ctx, const fx_trace_header_t *h)
{
    mem_t *m = (mem_t *)ctx;
    m->begins++;
    if (m->refuse_begin) {
        return false;
    }
    fx_trace_header_encode(h, m->buf);
    m->len = FX_TRACE_HEADER_LEN;
    return true;
}

static bool mem_write(void *ctx, const uint8_t sample[FX_TRACE_SAMPLE_LEN])
{
    mem_t *m = (mem_t *)ctx;
    m->writes++;
    if (m->fail_write_at && m->writes == m->fail_write_at) {
        return false;
    }
    if (m->len + FX_TRACE_SAMPLE_LEN > sizeof(m->buf)) {
        return false;
    }
    memcpy(m->buf + m->len, sample, FX_TRACE_SAMPLE_LEN);
    m->len += FX_TRACE_SAMPLE_LEN;
    return true;
}

static void mem_end(void *ctx, const fx_trace_header_t *h)
{
    mem_t *m = (mem_t *)ctx;
    m->ends++;
    fx_trace_header_encode(h, m->buf);
}

static void mem_abort(void *ctx)
{
    ((mem_t *)ctx)->aborts++;
}

static mem_t mem;
static const fx_trace_writer_t writer = {&mem, mem_begin, mem_write, mem_end, mem_abort};
static fx_trace_sample_t ring[4];
static fx_trace_t trace;

static fx_trace_sample_t sample(uint8_t state, uint16_t gas)
{
    fx_trace_sample_t s;
    memset(&s, 0, sizeof(s));
    s.score[FX_GAS] = gas;
    s.score[FX_HUMIDITY] = 450;
    s.state = state;
    s.alarm = 2;
    s.fan_duty = 100;
    s.fan_state = FX_TRACE_FAN_ON;
    return s;
}

static void start_small(void)
{
    memset(&mem, 0, sizeof(mem));
    const fx_trace_config_t cfg = {500, 3, 20};
    fx_trace_init(&trace, &cfg, ring, 4, 7);
}

static fx_trace_event_t feed(uint8_t state, uint16_t gas, uint32_t uptime_ms)
{
    const fx_trace_sample_t s = sample(state, gas);
    return fx_trace_feed(&trace, &s, uptime_ms, &writer);
}

/* --- format ------------------------------------------------------------------- */

static void test_scores_travel_as_thousandths_and_invalid_is_not_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(595, fx_trace_score(0.595f, true));
    TEST_ASSERT_EQUAL_UINT16(0, fx_trace_score(-0.1f, true));
    TEST_ASSERT_EQUAL_UINT16(1000, fx_trace_score(1.3f, true));
    TEST_ASSERT_EQUAL_UINT16(FX_TRACE_INVALID, fx_trace_score(0.0f, false));
}

static void test_header_and_sample_round_trip(void)
{
    const fx_trace_header_t h = {500, 600, 842, FX_STATE_CRITICAL, 123456789u, 42};
    uint8_t buf[FX_TRACE_HEADER_LEN];
    fx_trace_header_encode(&h, buf);
    TEST_ASSERT_EQUAL_MEMORY("FXTR\x01\x05", buf, 6);
    fx_trace_header_t back;
    TEST_ASSERT_TRUE(fx_trace_header_decode(buf, sizeof(buf), &back));
    TEST_ASSERT_EQUAL_UINT16(500, back.period_ms);
    TEST_ASSERT_EQUAL_UINT32(600, back.pre_samples);
    TEST_ASSERT_EQUAL_UINT32(842, back.samples);
    TEST_ASSERT_EQUAL_UINT8(FX_STATE_CRITICAL, back.trigger);
    TEST_ASSERT_EQUAL_UINT32(123456789u, back.trigger_uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(42, back.boot_count);
    TEST_ASSERT_FALSE(fx_trace_header_decode(buf, 31, &back));
    buf[4] = 2;
    TEST_ASSERT_FALSE(fx_trace_header_decode(buf, sizeof(buf), &back));

    fx_trace_sample_t s = sample(FX_STATE_ELEVATED, 650), t;
    s.score[FX_HEAT] = FX_TRACE_INVALID;
    uint8_t rec[FX_TRACE_SAMPLE_LEN];
    fx_trace_sample_encode(&s, rec);
    TEST_ASSERT_EQUAL_HEX8(0x8a, rec[0]); /* 650 = 0x028a */
    TEST_ASSERT_EQUAL_HEX8(0x02, rec[1]);
    TEST_ASSERT_EQUAL_HEX8(0xff, rec[6]);
    TEST_ASSERT_EQUAL_HEX8(0, rec[14]);
    fx_trace_sample_decode(rec, &t);
    TEST_ASSERT_EQUAL_MEMORY(&s, &t, sizeof(s));
}

/* --- episodes -------------------------------------------------------------------- */

static void test_nothing_is_written_while_normal(void)
{
    start_small();
    for (uint32_t i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_INT(FX_TRACE_IDLE, feed(FX_STATE_NORMAL, 50, i * 500u));
    }
    TEST_ASSERT_EQUAL_INT(0, mem.begins);
}

static void test_an_episode_carries_its_pre_window_and_ends_after_quiet(void)
{
    start_small();
    for (uint32_t i = 0; i < 6; i++) {
        feed(FX_STATE_NORMAL, (uint16_t)(10 + i), 1000u + i * 500u);
    }
    TEST_ASSERT_EQUAL_INT(FX_TRACE_STARTED, feed(FX_STATE_ELEVATED, 650, 4000u));
    TEST_ASSERT_EQUAL_INT(FX_TRACE_RECORDING, feed(FX_STATE_CRITICAL, 700, 4500u)); /* same file */
    TEST_ASSERT_EQUAL_INT(FX_TRACE_RECORDING, feed(FX_STATE_NORMAL, 50, 5000u));
    TEST_ASSERT_EQUAL_INT(FX_TRACE_RECORDING, feed(FX_STATE_ELEVATED, 640, 5500u)); /* quiet restarts */
    feed(FX_STATE_NORMAL, 50, 6000u);
    feed(FX_STATE_NORMAL, 50, 6500u);
    TEST_ASSERT_EQUAL_INT(FX_TRACE_ENDED, feed(FX_STATE_NORMAL, 50, 7000u));
    TEST_ASSERT_EQUAL_INT(1, mem.begins);
    TEST_ASSERT_EQUAL_INT(1, mem.ends);

    fx_trace_header_t h;
    TEST_ASSERT_TRUE(fx_trace_header_decode(mem.buf, mem.len, &h));
    TEST_ASSERT_EQUAL_UINT32(4, h.pre_samples);
    TEST_ASSERT_EQUAL_UINT32(11, h.samples);
    TEST_ASSERT_EQUAL_UINT8(FX_STATE_ELEVATED, h.trigger);
    TEST_ASSERT_EQUAL_UINT32(4000u, h.trigger_uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(7, h.boot_count);
    TEST_ASSERT_EQUAL_UINT(FX_TRACE_HEADER_LEN + 11u * FX_TRACE_SAMPLE_LEN, mem.len);

    fx_trace_sample_t first, trigger;
    fx_trace_sample_decode(mem.buf + FX_TRACE_HEADER_LEN, &first);
    TEST_ASSERT_EQUAL_UINT16(12, first.score[FX_GAS]); /* the oldest of the last four NORMAL samples */
    fx_trace_sample_decode(mem.buf + FX_TRACE_HEADER_LEN + 4u * FX_TRACE_SAMPLE_LEN, &trigger);
    TEST_ASSERT_EQUAL_UINT16(650, trigger.score[FX_GAS]);

    TEST_ASSERT_EQUAL_INT(FX_TRACE_IDLE, feed(FX_STATE_NORMAL, 50, 7500u));
}

static void test_the_cap_ends_a_file_and_a_continuing_alarm_starts_the_next(void)
{
    start_small();
    feed(FX_STATE_NORMAL, 50, 0u);
    TEST_ASSERT_EQUAL_INT(FX_TRACE_STARTED, feed(FX_STATE_CRITICAL, 900, 500u));
    fx_trace_event_t ev = FX_TRACE_IDLE;
    uint32_t t = 1000u;
    for (int i = 0; i < 18; i++, t += 500u) {
        ev = feed(FX_STATE_CRITICAL, 900, t);
    }
    TEST_ASSERT_EQUAL_INT(FX_TRACE_ENDED, ev); /* 1 pre + 19 = the 20-sample cap */
    fx_trace_header_t h;
    fx_trace_header_decode(mem.buf, mem.len, &h);
    TEST_ASSERT_EQUAL_UINT32(20, h.samples);
    TEST_ASSERT_EQUAL_INT(FX_TRACE_STARTED, feed(FX_STATE_CRITICAL, 900, t));
    TEST_ASSERT_EQUAL_INT(2, mem.begins);
    fx_trace_header_decode(mem.buf, mem.len, &h);
    TEST_ASSERT_EQUAL_UINT32(4, h.pre_samples); /* the last four CRITICAL samples of the file before */
}

static void test_a_refused_begin_drops_the_episode_once(void)
{
    start_small();
    mem.refuse_begin = true;
    TEST_ASSERT_EQUAL_INT(FX_TRACE_DROPPED, feed(FX_STATE_ELEVATED, 650, 0u));
    TEST_ASSERT_EQUAL_INT(FX_TRACE_IDLE, feed(FX_STATE_ELEVATED, 650, 500u)); /* followed, not retried */
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(FX_TRACE_IDLE, feed(FX_STATE_NORMAL, 50, 1000u + i * 500u));
    }
    TEST_ASSERT_EQUAL_INT(1, mem.begins);
    TEST_ASSERT_EQUAL_INT(0, mem.ends);
    mem.refuse_begin = false;
    TEST_ASSERT_EQUAL_INT(FX_TRACE_STARTED, feed(FX_STATE_ELEVATED, 650, 3000u)); /* the next episode */
}

static void test_a_failed_write_abandons_the_file(void)
{
    start_small();
    feed(FX_STATE_NORMAL, 50, 0u);
    mem.fail_write_at = 3; /* pre-window, trigger, then this one */
    TEST_ASSERT_EQUAL_INT(FX_TRACE_STARTED, feed(FX_STATE_ELEVATED, 650, 500u));
    TEST_ASSERT_EQUAL_INT(FX_TRACE_ABANDONED, feed(FX_STATE_ELEVATED, 650, 1000u));
    for (uint32_t i = 0; i < 3; i++) {
        feed(FX_STATE_NORMAL, 50, 1500u + i * 500u);
    }
    TEST_ASSERT_EQUAL_INT(1, mem.aborts);
    TEST_ASSERT_EQUAL_INT(0, mem.ends);
}

/* --- the golden file --------------------------------------------------------------- */

static void test_the_committed_golden_file_decodes(void)
{
    /* Written by `make trace-golden` (sim/trace_golden.c); read by the gateway and tools/pilot. */
    FILE *f = fopen("test/trace_golden.fxt", "rb");
    if (!f) {
        TEST_FAIL_MESSAGE("test/trace_golden.fxt is missing: run the tests from firmware/, or make trace-golden");
    }
    uint8_t buf[512];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    fx_trace_header_t h;
    TEST_ASSERT_TRUE(fx_trace_header_decode(buf, n, &h));
    TEST_ASSERT_EQUAL_UINT16(500, h.period_ms);
    TEST_ASSERT_EQUAL_UINT32(4, h.pre_samples);
    TEST_ASSERT_EQUAL_UINT32(10, h.samples);
    TEST_ASSERT_EQUAL_UINT8(FX_STATE_ELEVATED, h.trigger);
    TEST_ASSERT_EQUAL_UINT32(4000u, h.trigger_uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(7, h.boot_count);
    TEST_ASSERT_EQUAL_UINT(192, n);

    fx_trace_sample_t s;
    fx_trace_sample_decode(buf + FX_TRACE_HEADER_LEN + 4u * FX_TRACE_SAMPLE_LEN, &s);
    TEST_ASSERT_EQUAL_UINT16(650, s.score[FX_GAS]);
    TEST_ASSERT_EQUAL_UINT8(FX_STATE_ELEVATED, s.state);
    TEST_ASSERT_EQUAL_UINT8(3, s.alarm);
    fx_trace_sample_decode(buf + FX_TRACE_HEADER_LEN + 6u * FX_TRACE_SAMPLE_LEN, &s);
    TEST_ASSERT_EQUAL_UINT16(FX_TRACE_INVALID, s.score[FX_HUMIDITY]);
    TEST_ASSERT_EQUAL_UINT16(640, s.score[FX_HEAT]);
    TEST_ASSERT_EQUAL_UINT8(FX_STATE_CRITICAL, s.state);
    fx_trace_sample_decode(buf + FX_TRACE_HEADER_LEN + 9u * FX_TRACE_SAMPLE_LEN, &s);
    TEST_ASSERT_EQUAL_UINT8(FX_STATE_NORMAL, s.state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scores_travel_as_thousandths_and_invalid_is_not_zero);
    RUN_TEST(test_header_and_sample_round_trip);
    RUN_TEST(test_nothing_is_written_while_normal);
    RUN_TEST(test_an_episode_carries_its_pre_window_and_ends_after_quiet);
    RUN_TEST(test_the_cap_ends_a_file_and_a_continuing_alarm_starts_the_next);
    RUN_TEST(test_a_refused_begin_drops_the_episode_once);
    RUN_TEST(test_a_failed_write_abandons_the_file);
    RUN_TEST(test_the_committed_golden_file_decodes);
    return UNITY_END();
}
