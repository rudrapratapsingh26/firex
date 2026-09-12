/*
 * Host tests for the fusion engine (Unity). These run in CI with no board
 * attached: the library is pure C with no hardware includes.
 *
 *   gcc -I../components/fusion/include -Iunity ../components/fusion/fusion.c \
 *       unity/unity.c test_fusion.c -o test_fusion
 */
#include "unity.h"

#include "fusion.h"

#include <string.h>

#define WARM 180000u /* default warm-up, ms */

static fx_engine_t eng;
static fx_sample_t samples[FX_MODALITY_COUNT];

void setUp(void)
{
    fx_init(&eng, NULL, 0);
    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        samples[i].score = 0.0f;
        samples[i].valid = true;
    }
}

void tearDown(void) {}

/* Run the engine from `from_ms` to `to_ms` in 1 s steps and return the last result. */
static fx_result_t run_until(uint32_t from_ms, uint32_t to_ms)
{
    fx_result_t r;
    memset(&r, 0, sizeof(r));
    for (uint32_t t = from_ms; t <= to_ms; t += 1000u) {
        r = fx_update(&eng, samples, t);
    }
    return r;
}

/* --- warm-up ---------------------------------------------------------- */

static void test_warmup_blocks_every_alarm(void)
{
    samples[FX_GAS].score = 0.9f;
    samples[FX_HEAT].score = 0.9f;
    fx_result_t r = run_until(0, WARM - 2000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_WARMUP, r.state);
}

static void test_warmup_ends_at_the_configured_time(void)
{
    fx_result_t r = run_until(0, WARM + 1000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_NORMAL, r.state);
}

/* --- elevated --------------------------------------------------------- */

static void test_one_modality_held_five_seconds_is_elevated(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 6000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state);
    TEST_ASSERT_EQUAL_INT(FX_GAS, r.reason_a);
}

static void test_one_modality_below_the_dwell_is_still_normal(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 4000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_NORMAL, r.state);
}

static void test_a_score_under_the_enter_threshold_never_votes(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.59f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 30000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_NORMAL, r.state);
}

/* --- critical --------------------------------------------------------- */

static void test_two_modalities_held_ten_seconds_are_critical(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    samples[FX_HEAT].score = 0.7f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 12000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_CRITICAL, r.state);
    TEST_ASSERT_EQUAL_INT(2, r.voter_count);
    TEST_ASSERT_EQUAL_INT(FX_GAS, r.reason_a);
    TEST_ASSERT_EQUAL_INT(FX_HEAT, r.reason_b);
}

static void test_one_modality_never_reaches_critical_on_its_own(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.95f; /* high, but a single sensing physics */
    fx_result_t r = run_until(WARM + 1000u, WARM + 60000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state);
}

static void test_humidity_is_not_a_second_voter(void)
{
    run_until(0, WARM);
    /* Smoke, not gas: the steam rule damps gas specifically, and this test is
     * about humidity never counting as the second agreeing modality. */
    samples[FX_SMOKE].score = 0.8f;
    samples[FX_HUMIDITY].score = 0.95f; /* context only, never votes */
    fx_result_t r = run_until(WARM + 1000u, WARM + 20000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state);
    TEST_ASSERT_EQUAL_INT(1, r.voter_count);
}

static void test_flame_alone_is_critical_after_three_seconds(void)
{
    run_until(0, WARM);
    samples[FX_FLAME].score = 0.95f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 5000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_CRITICAL, r.state);
    TEST_ASSERT_EQUAL_INT(FX_FLAME, r.reason_a);
}

static void test_flame_alone_can_be_switched_off(void)
{
    fx_config_t cfg;
    fx_config_defaults(&cfg);
    cfg.flame_alone = false;
    fx_init(&eng, &cfg, 0);
    run_until(0, WARM);
    samples[FX_FLAME].score = 0.95f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 8000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state);
}

/* --- hysteresis ------------------------------------------------------- */

static void test_a_score_between_the_thresholds_holds_the_state(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    run_until(WARM + 1000u, WARM + 7000u);
    samples[FX_GAS].score = 0.5f; /* below enter, above exit: no chatter */
    fx_result_t r = run_until(WARM + 8000u, WARM + 12000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state);
}

static void test_a_score_below_the_exit_threshold_clears_the_state(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    run_until(WARM + 1000u, WARM + 7000u);
    samples[FX_GAS].score = 0.39f;
    fx_result_t r = run_until(WARM + 8000u, WARM + 10000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_NORMAL, r.state);
}

static void test_the_dwell_restarts_after_a_modality_clears(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    run_until(WARM + 1000u, WARM + 7000u);
    samples[FX_GAS].score = 0.2f;
    run_until(WARM + 8000u, WARM + 9000u);
    samples[FX_GAS].score = 0.7f;
    fx_result_t r = run_until(WARM + 10000u, WARM + 13000u); /* only 3 s held */
    TEST_ASSERT_EQUAL_INT(FX_STATE_NORMAL, r.state);
}

/* --- faults ----------------------------------------------------------- */

static void test_an_invalid_sensor_is_excluded_and_raises_fault(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.9f;
    samples[FX_GAS].valid = false; /* failed self-test: invalid, not zero */
    samples[FX_HEAT].score = 0.7f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 20000u);
    TEST_ASSERT_TRUE(r.fault);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state); /* heat still votes */
}

static void test_remaining_modalities_still_reach_critical_during_a_fault(void)
{
    run_until(0, WARM);
    samples[FX_HUMIDITY].valid = false;
    samples[FX_SMOKE].score = 0.7f;
    samples[FX_HEAT].score = 0.7f;
    fx_result_t r = run_until(WARM + 1000u, WARM + 12000u);
    TEST_ASSERT_TRUE(r.fault);
    TEST_ASSERT_EQUAL_INT(FX_STATE_CRITICAL, r.state);
}

/* --- humidity context ------------------------------------------------- */

static void test_steam_suppresses_a_gas_score_with_no_heat_rise(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;      /* would be ELEVATED on its own */
    samples[FX_HUMIDITY].score = 0.9f; /* bathroom steam */
    samples[FX_HEAT].score = 0.1f;     /* nothing is getting hotter */
    fx_result_t r = run_until(WARM + 1000u, WARM + 20000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_NORMAL, r.state);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.35f, r.effective[FX_GAS]);
}

static void test_steam_does_not_suppress_gas_once_heat_rises(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    samples[FX_HUMIDITY].score = 0.9f;
    samples[FX_HEAT].score = 0.5f; /* heat is rising: believe the gas sensor */
    fx_result_t r = run_until(WARM + 1000u, WARM + 20000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, r.state);
}

/* --- transitions ------------------------------------------------------ */

static void test_state_changed_is_reported_once_per_transition(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    run_until(WARM + 1000u, WARM + 5000u);
    fx_result_t changed = fx_update(&eng, samples, WARM + 6000u);
    fx_result_t steady = fx_update(&eng, samples, WARM + 7000u);
    TEST_ASSERT_TRUE(changed.state_changed);
    TEST_ASSERT_FALSE(steady.state_changed);
}

static void test_a_gas_leak_walks_normal_elevated_critical(void)
{
    run_until(0, WARM);
    samples[FX_GAS].score = 0.7f;
    fx_result_t elevated = run_until(WARM + 1000u, WARM + 6000u);
    samples[FX_HEAT].score = 0.7f;
    fx_result_t critical = run_until(WARM + 7000u, WARM + 18000u);
    TEST_ASSERT_EQUAL_INT(FX_STATE_ELEVATED, elevated.state);
    TEST_ASSERT_EQUAL_INT(FX_STATE_CRITICAL, critical.state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_warmup_blocks_every_alarm);
    RUN_TEST(test_warmup_ends_at_the_configured_time);
    RUN_TEST(test_one_modality_held_five_seconds_is_elevated);
    RUN_TEST(test_one_modality_below_the_dwell_is_still_normal);
    RUN_TEST(test_a_score_under_the_enter_threshold_never_votes);
    RUN_TEST(test_two_modalities_held_ten_seconds_are_critical);
    RUN_TEST(test_one_modality_never_reaches_critical_on_its_own);
    RUN_TEST(test_humidity_is_not_a_second_voter);
    RUN_TEST(test_flame_alone_is_critical_after_three_seconds);
    RUN_TEST(test_flame_alone_can_be_switched_off);
    RUN_TEST(test_a_score_between_the_thresholds_holds_the_state);
    RUN_TEST(test_a_score_below_the_exit_threshold_clears_the_state);
    RUN_TEST(test_the_dwell_restarts_after_a_modality_clears);
    RUN_TEST(test_an_invalid_sensor_is_excluded_and_raises_fault);
    RUN_TEST(test_remaining_modalities_still_reach_critical_during_a_fault);
    RUN_TEST(test_steam_suppresses_a_gas_score_with_no_heat_rise);
    RUN_TEST(test_steam_does_not_suppress_gas_once_heat_rises);
    RUN_TEST(test_state_changed_is_reported_once_per_transition);
    RUN_TEST(test_a_gas_leak_walks_normal_elevated_critical);
    return UNITY_END();
}
