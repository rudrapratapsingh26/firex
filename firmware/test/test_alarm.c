/* Host tests for components/alarm (Unity). */
#include "unity.h"

#include "fx_alarm.h"
#include "fx_debounce.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static fx_alarm_t alarm;

static fx_alarm_input_t in(fx_state_t fusion, uint32_t now_ms)
{
    fx_alarm_input_t i;
    memset(&i, 0, sizeof(i));
    i.have_fusion = true;
    i.fusion = fusion;
    i.now_ms = now_ms;
    return i;
}

static fx_alarm_output_t step(fx_state_t fusion, uint32_t now_ms)
{
    fx_alarm_input_t i = in(fusion, now_ms);
    return fx_alarm_step(&alarm, &i);
}

static fx_alarm_output_t press(fx_state_t fusion, uint32_t now_ms)
{
    fx_alarm_input_t i = in(fusion, now_ms);
    i.silence_pressed = true;
    return fx_alarm_step(&alarm, &i);
}

static void test_t3_pattern_timing(void)
{
    const uint32_t on[] = {0, 499, 1000, 1499, 2000, 2499, 4000, 4499};
    const uint32_t off[] = {500, 999, 1500, 1999, 2500, 3999, 4500};
    for (size_t k = 0; k < sizeof(on) / sizeof(on[0]); k++) {
        TEST_ASSERT_TRUE_MESSAGE(fx_t3_tone(on[k]), "expected tone");
    }
    for (size_t k = 0; k < sizeof(off) / sizeof(off[0]); k++) {
        TEST_ASSERT_FALSE_MESSAGE(fx_t3_tone(off[k]), "expected silence");
    }
}

static void test_boot_until_fusion_reports(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    fx_alarm_input_t i;
    memset(&i, 0, sizeof(i));
    fx_alarm_output_t o = fx_alarm_step(&alarm, &i);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_BOOT, o.state);
    TEST_ASSERT_FALSE(o.buzzer);
    o = step(FX_STATE_WARMUP, 100);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_WARMUP, o.state);
}

static void test_critical_sounds_immediately_in_t3_with_a_red_ring(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    step(FX_STATE_NORMAL, 0);
    fx_alarm_output_t o = step(FX_STATE_CRITICAL, 12345);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_CRITICAL, o.state);
    TEST_ASSERT_TRUE(o.buzzer);
    TEST_ASSERT_TRUE(o.ring.r > 0 && o.ring.g == 0 && o.ring.b == 0);
    TEST_ASSERT_FALSE(step(FX_STATE_CRITICAL, 12345 + 700).buzzer);
    TEST_ASSERT_TRUE(step(FX_STATE_CRITICAL, 12345 + 1200).buzzer);
}

static void test_silence_is_refused_outside_critical(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    step(FX_STATE_ELEVATED, 0);
    fx_alarm_output_t o = press(FX_STATE_ELEVATED, 100);
    TEST_ASSERT_TRUE(o.silence_rejected);
    TEST_ASSERT_FALSE(o.silence_started);
    o = step(FX_STATE_CRITICAL, 200);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_CRITICAL, o.state); /* an earlier press does not pre-silence */
    TEST_ASSERT_TRUE(o.buzzer);
}

static void test_silence_mutes_then_resumes_if_still_critical(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    step(FX_STATE_CRITICAL, 1000);
    fx_alarm_output_t o = press(FX_STATE_CRITICAL, 2000);
    TEST_ASSERT_TRUE(o.silence_started);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_SILENCED, o.state);
    TEST_ASSERT_FALSE(o.buzzer);
    TEST_ASSERT_TRUE(o.ring.r > 0); /* ring stays red */

    o = press(FX_STATE_CRITICAL, 2000 + 120000); /* a second press does not extend the window */
    TEST_ASSERT_FALSE(o.silence_started);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_SILENCED, step(FX_STATE_CRITICAL, 2000 + 299999).state);

    o = step(FX_STATE_CRITICAL, 2000 + 300000);
    TEST_ASSERT_TRUE(o.silence_ended);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_CRITICAL, o.state);
    TEST_ASSERT_TRUE(o.buzzer); /* resumes at the start of the pattern */
}

static void test_leaving_critical_clears_the_silence(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    step(FX_STATE_CRITICAL, 0);
    press(FX_STATE_CRITICAL, 10);
    fx_alarm_output_t o = step(FX_STATE_ELEVATED, 5000);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_ELEVATED, o.state);
    TEST_ASSERT_FALSE(o.silence_ended);
    o = step(FX_STATE_CRITICAL, 6000);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_CRITICAL, o.state);
    TEST_ASSERT_TRUE(o.buzzer);
}

static void test_fault_blinks_but_never_hides_a_hazard(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    fx_alarm_input_t i = in(FX_STATE_NORMAL, 0);
    i.fault = true;
    fx_alarm_output_t o = fx_alarm_step(&alarm, &i);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_FAULT, o.state);
    TEST_ASSERT_TRUE(o.ring.r > 0);
    i.now_ms = 600;
    TEST_ASSERT_EQUAL_UINT8(0, fx_alarm_step(&alarm, &i).ring.r); /* blink off */

    i.fusion = FX_STATE_ELEVATED;
    TEST_ASSERT_EQUAL_INT(FX_ALARM_ELEVATED, fx_alarm_step(&alarm, &i).state);
    i.fusion = FX_STATE_CRITICAL;
    o = fx_alarm_step(&alarm, &i);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_CRITICAL, o.state);
    TEST_ASSERT_TRUE(o.buzzer);
}

static void test_self_test_and_idle_colours(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    fx_alarm_input_t i = in(FX_STATE_NORMAL, 0);
    fx_alarm_output_t normal = fx_alarm_step(&alarm, &i);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_NORMAL, normal.state);
    TEST_ASSERT_TRUE(normal.ring.g > normal.ring.r);
    i.self_test = true;
    i.now_ms = 10;
    fx_alarm_output_t st = fx_alarm_step(&alarm, &i);
    TEST_ASSERT_EQUAL_INT(FX_ALARM_SELF_TEST, st.state);
    TEST_ASSERT_TRUE(st.ring.b > st.ring.r);
    TEST_ASSERT_FALSE(st.buzzer);
}

static void test_warmup_pulses(void)
{
    fx_alarm_init(&alarm, NULL, 0);
    const uint8_t dim = step(FX_STATE_WARMUP, 0).ring.r;
    const uint8_t bright = step(FX_STATE_WARMUP, 1000).ring.r;
    TEST_ASSERT_TRUE(bright > dim);
    TEST_ASSERT_TRUE(dim > 0);
}

static void test_debounce_ignores_bounce_and_glitches(void)
{
    fx_debounce_t d;
    fx_debounce_init(&d, 30, false);
    int presses = 0;
    const bool bounce[] = {true, false, true, false, true, true, true, true, true, true};
    uint32_t t = 0;
    for (size_t k = 0; k < sizeof(bounce) / sizeof(bounce[0]); k++, t += 10) {
        presses += fx_debounce_press(&d, bounce[k], t) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(1, presses);
    for (int k = 0; k < 10; k++, t += 10) {
        presses += fx_debounce_press(&d, true, t) ? 1 : 0; /* holding is not a second press */
    }
    TEST_ASSERT_EQUAL_INT(1, presses);

    fx_debounce_init(&d, 30, false);
    TEST_ASSERT_FALSE(fx_debounce_press(&d, true, 1000));
    TEST_ASSERT_FALSE(fx_debounce_press(&d, true, 1020));
    TEST_ASSERT_FALSE(fx_debounce_press(&d, false, 1025)); /* a 25 ms brush */
    TEST_ASSERT_FALSE(fx_debounce_press(&d, false, 1100));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_t3_pattern_timing);
    RUN_TEST(test_boot_until_fusion_reports);
    RUN_TEST(test_critical_sounds_immediately_in_t3_with_a_red_ring);
    RUN_TEST(test_silence_is_refused_outside_critical);
    RUN_TEST(test_silence_mutes_then_resumes_if_still_critical);
    RUN_TEST(test_leaving_critical_clears_the_silence);
    RUN_TEST(test_fault_blinks_but_never_hides_a_hazard);
    RUN_TEST(test_self_test_and_idle_colours);
    RUN_TEST(test_warmup_pulses);
    RUN_TEST(test_debounce_ignores_bounce_and_glitches);
    return UNITY_END();
}
