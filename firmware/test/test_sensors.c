/*
 * Host tests for the hardware-independent half of components/sensors (Unity).
 * Everything here runs in CI with no board: register arithmetic, scoring,
 * frame decoding and the common score API. The ESP-IDF drivers in hw/ are thin
 * wrappers that feed these functions.
 */
#include "unity.h"

#include "ads1115_reg.h"
#include "dht22_frame.h"
#include "fan_health.h"
#include "flame_score.h"
#include "fx_sensor.h"
#include "heat_score.h"
#include "mq_score.h"
#include "ntc_score.h"

#include <math.h>
#include <string.h>

#define TWO_PI 6.2831853

void setUp(void) {}
void tearDown(void) {}

/* --- ADS1115 ---------------------------------------------------------- */

static void test_ads1115_config_word_for_a_single_shot_on_ain0(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xC383, fx_ads1115_single_shot_config(0, FX_ADS_PGA_4V096, FX_ADS_DR_128));
    TEST_ASSERT_EQUAL_HEX16(0xF383, fx_ads1115_single_shot_config(3, FX_ADS_PGA_4V096, FX_ADS_DR_128));
    TEST_ASSERT_EQUAL_HEX16(0x0000, fx_ads1115_single_shot_config(4, FX_ADS_PGA_4V096, FX_ADS_DR_128));
}

static void test_ads1115_counts_to_volts(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.048f, fx_ads1115_volts(16384, FX_ADS_PGA_4V096));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.096f, fx_ads1115_volts(-32768, FX_ADS_PGA_4V096));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.000125f, fx_ads1115_volts(1, FX_ADS_PGA_4V096));
}

static void test_ads1115_conversion_timing_and_ready_bit(void)
{
    TEST_ASSERT_EQUAL_UINT32(9, fx_ads1115_conversion_ms(FX_ADS_DR_128));
    TEST_ASSERT_EQUAL_UINT32(3, fx_ads1115_conversion_ms(FX_ADS_DR_860));
    TEST_ASSERT_EQUAL_UINT32(126, fx_ads1115_conversion_ms(FX_ADS_DR_8));
    TEST_ASSERT_TRUE(fx_ads1115_conversion_ready(0x8583));
    TEST_ASSERT_FALSE(fx_ads1115_conversion_ready(0x0583));
}

/* --- MQ gas sensors --------------------------------------------------- */

static fx_mq_params_t plain_mq(void)
{
    fx_mq_params_t p;
    fx_mq2_defaults(&p);
    p.divider = 1.0f;
    return p;
}

static void test_mq_rs_from_the_load_divider(void)
{
    fx_mq_params_t p = plain_mq();
    float rs = 0.0f;
    TEST_ASSERT_TRUE(fx_mq_rs_ohms(&p, 1.0f, &rs));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 4000.0f, rs);

    fx_mq2_defaults(&p); /* 2/3 divider in front of the ADC */
    TEST_ASSERT_TRUE(fx_mq_rs_ohms(&p, 2.0f / 3.0f, &rs));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 4000.0f, rs);
}

static void test_mq_a_reading_at_either_rail_is_not_a_resistance(void)
{
    fx_mq_params_t p = plain_mq();
    float rs = 0.0f;
    TEST_ASSERT_FALSE(fx_mq_rs_ohms(&p, 0.0f, &rs)); /* AO disconnected */
    TEST_ASSERT_FALSE(fx_mq_rs_ohms(&p, 5.0f, &rs)); /* shorted to supply */
}

static void test_mq_score_is_log_linear_in_the_ratio(void)
{
    fx_mq_params_t p = plain_mq(); /* ratio_full 0.10 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, fx_mq_score(&p, 10000.0f, 10000.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, fx_mq_score(&p, 15000.0f, 10000.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, fx_mq_score(&p, 1000.0f, 10000.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, fx_mq_score(&p, 3162.3f, 10000.0f));
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.6f, fx_mq_score(&p, 2512.0f, 10000.0f));
}

static void test_mq_sample_is_invalid_without_a_baseline(void)
{
    fx_mq_params_t p = plain_mq();
    float rs = 0.0f, ratio = 0.0f;
    fx_sample_t s = fx_mq_sample(&p, 1.0f, 0.0f, &rs, &ratio);
    TEST_ASSERT_FALSE(s.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 4000.0f, rs); /* Rs still reported for capture */

    s = fx_mq_sample(&p, 1.0f, 4000.0f, &rs, &ratio);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, ratio);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, s.score);

    s = fx_mq_sample(&p, 0.0f, 4000.0f, NULL, NULL);
    TEST_ASSERT_FALSE(s.valid);
}

static void test_mq_baseline_median_in_settled_air(void)
{
    fx_mq_baseline_t b;
    fx_mq_baseline_begin(&b, 30, 0.05f);
    float r0 = 0.0f, spread = 1.0f;
    for (int i = 0; i < 29; i++) {
        TEST_ASSERT_EQUAL_INT(FX_MQ_BASELINE_COLLECTING,
                              fx_mq_baseline_add(&b, 10000.0f + (float)(i % 3) * 10.0f, &r0, &spread));
    }
    TEST_ASSERT_EQUAL_INT(FX_MQ_BASELINE_READY, fx_mq_baseline_add(&b, 10010.0f, &r0, &spread));
    TEST_ASSERT_FLOAT_WITHIN(10.0f, 10010.0f, r0);
    TEST_ASSERT_TRUE(spread < 0.01f);
}

static void test_mq_baseline_ignores_one_spike(void)
{
    fx_mq_baseline_t b;
    fx_mq_baseline_begin(&b, 30, 0.05f);
    fx_mq_baseline_status_t st = FX_MQ_BASELINE_COLLECTING;
    float r0 = 0.0f;
    for (int i = 0; i < 30; i++) {
        st = fx_mq_baseline_add(&b, i == 12 ? 50000.0f : 10000.0f, &r0, NULL);
    }
    TEST_ASSERT_EQUAL_INT(FX_MQ_BASELINE_READY, st);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 10000.0f, r0);
}

static void test_mq_baseline_rejects_a_drifting_heater(void)
{
    fx_mq_baseline_t b;
    fx_mq_baseline_begin(&b, 30, 0.05f);
    fx_mq_baseline_status_t st = FX_MQ_BASELINE_COLLECTING;
    for (int i = 0; i < 30; i++) {
        st = fx_mq_baseline_add(&b, 10000.0f + 100.0f * (float)i, NULL, NULL);
    }
    TEST_ASSERT_EQUAL_INT(FX_MQ_BASELINE_UNSTABLE, st);
}

static void test_mq_tracking_follows_drift_but_never_learns_gas(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 10050.0f, fx_mq_baseline_track(10000.0f, 10500.0f, 0.1f, 0.2f));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 10000.0f, fx_mq_baseline_track(10000.0f, 3000.0f, 0.1f, 0.2f));
}

/* --- flame flicker ---------------------------------------------------- */

static fx_flame_t flame;

static void feed_sine(double seconds, double hz, double amplitude, double phase, double dc)
{
    const int n = (int)(seconds * 100.0);
    for (int k = 0; k < n; k++) {
        const double t = (double)k / 100.0;
        double v = dc + amplitude * sin(TWO_PI * hz * t + phase);
        v = v < 0.0 ? 0.0 : (v > 4095.0 ? 4095.0 : v);
        fx_flame_push(&flame, (uint16_t)(v + 0.5));
    }
}

static void test_flame_steady_light_scores_zero(void)
{
    fx_flame_init(&flame, NULL);
    feed_sine(4.0, 0.0, 0.0, 0.0, 2000.0);
    fx_flame_result_t r = fx_flame_evaluate(&flame);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.score);
}

static void test_flame_flicker_in_band_scores_high(void)
{
    fx_flame_init(&flame, NULL);
    feed_sine(4.0, 8.0, 200.0, 0.3, 2000.0);
    fx_flame_result_t r = fx_flame_evaluate(&flame);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE(r.score >= 0.9f);
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 8.0f, r.flicker_hz);
}

static void test_flame_slow_swing_below_the_band_scores_zero(void)
{
    fx_flame_init(&flame, NULL);
    feed_sine(10.0, 0.2, 500.0, 0.0, 2000.0); /* a cloud, or someone walking past */
    fx_flame_result_t r = fx_flame_evaluate(&flame);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.score);
}

static void test_flame_mains_flicker_aliased_above_the_band_scores_zero(void)
{
    fx_flame_init(&flame, NULL);
    feed_sine(4.0, 50.0, 300.0, 0.7, 2000.0); /* 50 Hz sampled at 100 Hz */
    fx_flame_result_t r = fx_flame_evaluate(&flame);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE(r.flicker_hz > 15.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.score);
}

static void test_flame_a_window_pinned_to_a_rail_is_invalid(void)
{
    fx_flame_init(&flame, NULL);
    feed_sine(3.0, 0.0, 0.0, 0.0, 4095.0);
    TEST_ASSERT_FALSE(fx_flame_evaluate(&flame).valid);
    fx_flame_init(&flame, NULL);
    feed_sine(3.0, 0.0, 0.0, 0.0, 0.0);
    TEST_ASSERT_FALSE(fx_flame_evaluate(&flame).valid);
}

static void test_flame_first_window_is_valid_and_quiet(void)
{
    fx_flame_init(&flame, NULL);
    feed_sine(1.0, 8.0, 200.0, 0.0, 2000.0);
    fx_flame_result_t r = fx_flame_evaluate(&flame);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.score);
}

/* --- DHT22 ------------------------------------------------------------ */

static void test_dht22_decodes_a_known_frame(void)
{
    const uint8_t bytes[5] = {0x02, 0x8C, 0x01, 0x5F, 0xEE};
    fx_dht22_reading_t r;
    TEST_ASSERT_EQUAL_INT(FX_DHT22_OK, fx_dht22_decode_bytes(bytes, &r));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 65.2f, r.rh_pct);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 35.1f, r.temp_c);
}

static void test_dht22_sign_bit_gives_a_negative_temperature(void)
{
    const uint8_t bytes[5] = {0x02, 0x8C, 0x80, 0x65, 0x73};
    fx_dht22_reading_t r;
    TEST_ASSERT_EQUAL_INT(FX_DHT22_OK, fx_dht22_decode_bytes(bytes, &r));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -10.1f, r.temp_c);
}

static void test_dht22_rejects_checksum_zero_frame_and_range(void)
{
    fx_dht22_reading_t r;
    const uint8_t bad_sum[5] = {0x02, 0x8C, 0x01, 0x5F, 0xEF};
    const uint8_t zeros[5] = {0, 0, 0, 0, 0};
    const uint8_t wet[5] = {0x03, 0xE9, 0x00, 0xFA, 0xE6}; /* 100.1 % RH */
    TEST_ASSERT_EQUAL_INT(FX_DHT22_CHECKSUM, fx_dht22_decode_bytes(bad_sum, &r));
    TEST_ASSERT_EQUAL_INT(FX_DHT22_ZERO_FRAME, fx_dht22_decode_bytes(zeros, &r));
    TEST_ASSERT_EQUAL_INT(FX_DHT22_RANGE, fx_dht22_decode_bytes(wet, &r));
}

/* Build the pulse train a DHT22 sends for five bytes, with the tail of the host
 * start signal and the 80 us response pair in front. */
static size_t build_pulses(const uint8_t bytes[5], fx_pulse_t *out, size_t bits, uint16_t one_us)
{
    size_t n = 0;
    out[n++] = (fx_pulse_t){1, 30};
    out[n++] = (fx_pulse_t){0, 80};
    out[n++] = (fx_pulse_t){1, 80};
    for (size_t bit = 0; bit < bits; bit++) {
        const int one = (bytes[bit / 8] >> (7 - bit % 8)) & 1;
        out[n++] = (fx_pulse_t){0, 50};
        out[n++] = (fx_pulse_t){1, one ? one_us : 27};
    }
    out[n++] = (fx_pulse_t){0, 50};
    out[n++] = (fx_pulse_t){1, 0}; /* idle high that ended the capture */
    return n;
}

static void test_dht22_decodes_a_pulse_train(void)
{
    const uint8_t bytes[5] = {0x02, 0x8C, 0x01, 0x5F, 0xEE};
    fx_pulse_t pulses[100];
    const size_t n = build_pulses(bytes, pulses, 40, 70);
    fx_dht22_reading_t r;
    TEST_ASSERT_EQUAL_INT(FX_DHT22_OK, fx_dht22_decode_pulses(pulses, n, &r));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 65.2f, r.rh_pct);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 35.1f, r.temp_c);
}

static void test_dht22_short_capture_and_bad_pulse(void)
{
    const uint8_t bytes[5] = {0x02, 0x8C, 0x01, 0x5F, 0xEE};
    fx_pulse_t pulses[100];
    fx_dht22_reading_t r;
    size_t n = build_pulses(bytes, pulses, 30, 70);
    TEST_ASSERT_EQUAL_INT(FX_DHT22_SHORT_FRAME, fx_dht22_decode_pulses(pulses, n, &r));
    n = build_pulses(bytes, pulses, 40, 150);
    TEST_ASSERT_EQUAL_INT(FX_DHT22_BAD_PULSE, fx_dht22_decode_pulses(pulses, n, &r));
}

static void test_dht22_two_second_floor(void)
{
    TEST_ASSERT_TRUE(fx_dht22_due(false, 0u, 0u));
    TEST_ASSERT_FALSE(fx_dht22_due(true, 1000u, 2999u));
    TEST_ASSERT_TRUE(fx_dht22_due(true, 1000u, 3000u));
    TEST_ASSERT_TRUE(fx_dht22_due(true, 0xFFFFF000u, 0x00000800u)); /* clock wrap */
}

/* --- heat and humidity ------------------------------------------------ */

static fx_heat_t heat;

static void test_heat_steady_room_scores_zero(void)
{
    fx_heat_init(&heat, NULL);
    for (uint32_t t = 0; t <= 60000u; t += 2000u) {
        fx_heat_push(&heat, t, 25.0f, 50.0f);
    }
    fx_heat_result_t r = fx_heat_evaluate(&heat, 60000u);
    TEST_ASSERT_TRUE(r.heat.valid);
    TEST_ASSERT_TRUE(r.ror_known);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, r.ror_c_min);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, r.heat.score);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, r.humidity.score);
}

static void test_heat_rate_of_rise_scores_before_the_ceiling(void)
{
    fx_heat_init(&heat, NULL);
    for (uint32_t t = 0; t <= 60000u; t += 2000u) {
        fx_heat_push(&heat, t, 25.0f + 0.2f * (float)t / 1000.0f, 40.0f); /* 12 C/min */
    }
    fx_heat_result_t r = fx_heat_evaluate(&heat, 60000u);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.0f, r.ror_c_min);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, r.heat.score); /* still only 37 C */
}

static void test_heat_missed_reads_barely_move_the_slope(void)
{
    fx_heat_init(&heat, NULL);
    for (uint32_t t = 0; t <= 60000u; t += 2000u) {
        if ((t / 2000u) % 3u == 1u) {
            continue; /* every third read failed */
        }
        fx_heat_push(&heat, t, 25.0f + 0.2f * (float)t / 1000.0f, 40.0f);
    }
    fx_heat_result_t r = fx_heat_evaluate(&heat, 60000u);
    TEST_ASSERT_TRUE(r.ror_known);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.0f, r.ror_c_min);
}

static void test_heat_absolute_term_alone(void)
{
    fx_heat_init(&heat, NULL);
    fx_heat_push(&heat, 0u, 51.0f, 30.0f);
    fx_heat_push(&heat, 2000u, 51.0f, 30.0f);
    fx_heat_result_t r = fx_heat_evaluate(&heat, 2000u);
    TEST_ASSERT_TRUE(r.heat.valid);
    TEST_ASSERT_FALSE(r.ror_known); /* two readings are not a rate */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, r.heat.score);
}

static void test_heat_a_silent_sensor_is_invalid_not_cold(void)
{
    fx_heat_init(&heat, NULL);
    fx_heat_push(&heat, 0u, 25.0f, 50.0f);
    TEST_ASSERT_TRUE(fx_heat_evaluate(&heat, 10000u).heat.valid);
    fx_heat_result_t r = fx_heat_evaluate(&heat, 10001u);
    TEST_ASSERT_FALSE(r.heat.valid);
    TEST_ASSERT_FALSE(r.humidity.valid);
    fx_heat_init(&heat, NULL);
    TEST_ASSERT_FALSE(fx_heat_evaluate(&heat, 0u).heat.valid);
}

static void test_heat_thermistor_cadence_fills_a_sixty_point_window(void)
{
    fx_heat_init(&heat, NULL);
    for (uint32_t i = 0; i < 60u; i++) {
        const uint32_t t = i * 500u;
        fx_heat_push(&heat, t, 25.0f + 0.1f * (float)t / 1000.0f, 0.0f); /* 6 C/min */
    }
    TEST_ASSERT_EQUAL_UINT8(60, heat.count); /* all of them, not the last 32 */
    fx_heat_result_t r = fx_heat_evaluate(&heat, 29500u);
    TEST_ASSERT_TRUE(r.ror_known);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 6.0f, r.ror_c_min);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 3.0f / 7.0f, r.heat.score); /* ror term (6-3)/(10-3); 27.95 C is below 45 */
}

static void test_heat_window_wraps_past_its_capacity(void)
{
    fx_heat_init(&heat, NULL);
    for (uint32_t i = 0; i < 150u; i++) {
        const uint32_t t = i * 500u;
        fx_heat_push(&heat, t, 25.0f + 0.1f * (float)t / 1000.0f, 0.0f);
    }
    TEST_ASSERT_EQUAL_UINT8(FX_HEAT_WINDOW_MAX, heat.count);
    fx_heat_result_t r = fx_heat_evaluate(&heat, 74500u);
    TEST_ASSERT_TRUE(r.ror_known);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 6.0f, r.ror_c_min);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 32.45f, r.temp_c);
}

/* --- NTC thermistor (EVT) --------------------------------------------- */

static void test_ntc_at_25c_reads_r25(void)
{
    fx_ntc_params_t p;
    fx_ntc_defaults(&p);
    float ohms = 0.0f, temp_c = -1.0f;
    TEST_ASSERT_TRUE(fx_ntc_ohms(&p, 1.65f, &ohms)); /* half the supply: equal resistors */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 10000.0f, ohms);
    TEST_ASSERT_TRUE(fx_ntc_temp_c(&p, 10000.0f, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, temp_c);
    TEST_ASSERT_EQUAL_INT(FX_NTC_OK, fx_ntc_read(&p, 1.65f, &ohms, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, temp_c);
}

static void test_ntc_beta_points_at_0_and_85c(void)
{
    /* By hand, B = 3950, R25 = 10 k:
     *   0 C:  1/273.15 - 1/298.15 = 3.0697e-4;  x 3950 = 1.21253;  e^1.21253 = 3.3620  -> 33620 ohm
     *   85 C: 1/358.15 - 1/298.15 = -5.6189e-4; x 3950 = -2.21947; e^-2.21947 = 0.10867 -> 1086.7 ohm
     * At 0 C the divider gives 3.3 x 33620 / 43620 = 2.5435 V. */
    fx_ntc_params_t p;
    fx_ntc_defaults(&p);
    float temp_c = 0.0f, ohms = 0.0f;
    TEST_ASSERT_TRUE(fx_ntc_temp_c(&p, 33620.0f, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, temp_c);
    TEST_ASSERT_TRUE(fx_ntc_temp_c(&p, 1086.7f, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 85.0f, temp_c);
    TEST_ASSERT_TRUE(fx_ntc_ohms(&p, 2.5435f, &ohms));
    TEST_ASSERT_FLOAT_WITHIN(10.0f, 33620.0f, ohms);
    /* A 10 k / 3950 part's own table says 32.65 k at 0 C: the Beta model reads it as
     * about 0.6 C, the error calibration checks with a reference thermometer. */
    TEST_ASSERT_TRUE(fx_ntc_temp_c(&p, 32650.0f, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.6f, temp_c);
}

static void test_ntc_open_and_short_are_not_readings(void)
{
    fx_ntc_params_t p;
    fx_ntc_defaults(&p);
    float ohms = 123.0f, temp_c = 45.0f;
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, 0.0f, &ohms));
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, 0.005f, &ohms));
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, -0.2f, &ohms));
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, 3.3f, &ohms));
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, 3.295f, &ohms));
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, 4.0f, &ohms));
    TEST_ASSERT_FALSE(fx_ntc_ohms(&p, NAN, &ohms));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 123.0f, ohms); /* untouched */

    TEST_ASSERT_EQUAL_INT(FX_NTC_OPEN, fx_ntc_read(&p, 3.3f, &ohms, &temp_c));
    TEST_ASSERT_EQUAL_INT(FX_NTC_OPEN, fx_ntc_read(&p, 4.0f, &ohms, &temp_c));
    TEST_ASSERT_EQUAL_INT(FX_NTC_SHORT, fx_ntc_read(&p, 0.0f, &ohms, &temp_c));
    TEST_ASSERT_EQUAL_INT(FX_NTC_SHORT, fx_ntc_read(&p, NAN, &ohms, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 45.0f, temp_c); /* never a temperature */
    TEST_ASSERT_EQUAL_STRING("open", fx_ntc_status_name(FX_NTC_OPEN));
}

static void test_ntc_out_of_range_is_invalid(void)
{
    fx_ntc_params_t p;
    fx_ntc_defaults(&p);
    float temp_c = 45.0f, ohms = 0.0f;
    TEST_ASSERT_FALSE(fx_ntc_temp_c(&p, 1.0e6f, &temp_c)); /* about -52 C */
    TEST_ASSERT_FALSE(fx_ntc_temp_c(&p, 100.0f, &temp_c)); /* about 184 C */
    TEST_ASSERT_FALSE(fx_ntc_temp_c(&p, 0.0f, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 45.0f, temp_c);
    /* The same resistances through the divider: off the rails, but not a room. */
    TEST_ASSERT_EQUAL_INT(FX_NTC_OUT_OF_RANGE, fx_ntc_read(&p, 3.2673f, &ohms, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(5000.0f, 1.0e6f, ohms);
    TEST_ASSERT_EQUAL_INT(FX_NTC_OUT_OF_RANGE, fx_ntc_read(&p, 0.03267f, &ohms, &temp_c));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 45.0f, temp_c);
    TEST_ASSERT_EQUAL_STRING("out_of_range", fx_ntc_status_name(FX_NTC_OUT_OF_RANGE));
}

/* --- fan -------------------------------------------------------------- */

static void test_fan_rpm_and_health(void)
{
    fx_fan_params_t p;
    fx_fan_defaults(&p);
    TEST_ASSERT_EQUAL_UINT32(3000, fx_fan_rpm(100, 1000, 2));
    TEST_ASSERT_EQUAL_UINT32(0, fx_fan_rpm(100, 0, 2));
    TEST_ASSERT_EQUAL_INT(FX_FAN_OFF, fx_fan_evaluate(&p, 0, true, 900, 10000));
    TEST_ASSERT_EQUAL_INT(FX_FAN_ON, fx_fan_evaluate(&p, 60, false, 0, 10000));
    TEST_ASSERT_EQUAL_INT(FX_FAN_ON, fx_fan_evaluate(&p, 60, true, 0, 1000)); /* spinning up */
    TEST_ASSERT_EQUAL_INT(FX_FAN_FAILED, fx_fan_evaluate(&p, 60, true, 0, 5000));
    TEST_ASSERT_EQUAL_INT(FX_FAN_ON, fx_fan_evaluate(&p, 60, true, 1200, 5000));
    TEST_ASSERT_EQUAL_STRING("failed", fx_fan_state_name(FX_FAN_FAILED));
}

/* --- common score API ------------------------------------------------- */

static void fake_gas_score(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT])
{
    (void)s;
    (void)now_ms;
    samples[FX_GAS].score = 0.7f;
    samples[FX_GAS].valid = true;
    samples[FX_FLAME].score = 0.9f; /* not its modality */
    samples[FX_FLAME].valid = true;
}

static void fake_dht_score(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT])
{
    (void)s;
    (void)now_ms;
    samples[FX_HEAT].score = 0.2f;
    samples[FX_HEAT].valid = true;
    samples[FX_HUMIDITY].score = 0.5f;
    samples[FX_HUMIDITY].valid = true;
}

static void test_collect_keeps_each_driver_to_its_own_modalities(void)
{
    fx_sensor_t gas;
    fx_sensor_t dht;
    memset(&gas, 0, sizeof(gas));
    memset(&dht, 0, sizeof(dht));
    gas.name = "mq2";
    gas.modalities = FX_MOD_BIT(FX_GAS);
    gas.score = fake_gas_score;
    gas.ready = true;
    dht.name = "dht22";
    dht.modalities = (uint8_t)(FX_MOD_BIT(FX_HEAT) | FX_MOD_BIT(FX_HUMIDITY));
    dht.score = fake_dht_score;
    dht.ready = false;

    fx_sensor_t *const list[] = {&gas, &dht};
    fx_sample_t out[FX_MODALITY_COUNT];

    uint8_t covered = fx_sensors_collect(list, 2, 0u, out);
    TEST_ASSERT_EQUAL_HEX8(FX_MOD_BIT(FX_GAS), covered);
    TEST_ASSERT_TRUE(out[FX_GAS].valid);
    TEST_ASSERT_FALSE(out[FX_FLAME].valid); /* written by the wrong driver: ignored */
    TEST_ASSERT_FALSE(out[FX_HEAT].valid);  /* driver not ready: invalid, never zero */

    dht.ready = true;
    covered = fx_sensors_collect(list, 2, 0u, out);
    TEST_ASSERT_EQUAL_HEX8(FX_MOD_BIT(FX_GAS) | FX_MOD_BIT(FX_HEAT) | FX_MOD_BIT(FX_HUMIDITY), covered);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, out[FX_HUMIDITY].score);
    TEST_ASSERT_FALSE(out[FX_SMOKE].valid);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ads1115_config_word_for_a_single_shot_on_ain0);
    RUN_TEST(test_ads1115_counts_to_volts);
    RUN_TEST(test_ads1115_conversion_timing_and_ready_bit);
    RUN_TEST(test_mq_rs_from_the_load_divider);
    RUN_TEST(test_mq_a_reading_at_either_rail_is_not_a_resistance);
    RUN_TEST(test_mq_score_is_log_linear_in_the_ratio);
    RUN_TEST(test_mq_sample_is_invalid_without_a_baseline);
    RUN_TEST(test_mq_baseline_median_in_settled_air);
    RUN_TEST(test_mq_baseline_ignores_one_spike);
    RUN_TEST(test_mq_baseline_rejects_a_drifting_heater);
    RUN_TEST(test_mq_tracking_follows_drift_but_never_learns_gas);
    RUN_TEST(test_flame_steady_light_scores_zero);
    RUN_TEST(test_flame_flicker_in_band_scores_high);
    RUN_TEST(test_flame_slow_swing_below_the_band_scores_zero);
    RUN_TEST(test_flame_mains_flicker_aliased_above_the_band_scores_zero);
    RUN_TEST(test_flame_a_window_pinned_to_a_rail_is_invalid);
    RUN_TEST(test_flame_first_window_is_valid_and_quiet);
    RUN_TEST(test_dht22_decodes_a_known_frame);
    RUN_TEST(test_dht22_sign_bit_gives_a_negative_temperature);
    RUN_TEST(test_dht22_rejects_checksum_zero_frame_and_range);
    RUN_TEST(test_dht22_decodes_a_pulse_train);
    RUN_TEST(test_dht22_short_capture_and_bad_pulse);
    RUN_TEST(test_dht22_two_second_floor);
    RUN_TEST(test_heat_steady_room_scores_zero);
    RUN_TEST(test_heat_rate_of_rise_scores_before_the_ceiling);
    RUN_TEST(test_heat_missed_reads_barely_move_the_slope);
    RUN_TEST(test_heat_absolute_term_alone);
    RUN_TEST(test_heat_a_silent_sensor_is_invalid_not_cold);
    RUN_TEST(test_heat_thermistor_cadence_fills_a_sixty_point_window);
    RUN_TEST(test_heat_window_wraps_past_its_capacity);
    RUN_TEST(test_ntc_at_25c_reads_r25);
    RUN_TEST(test_ntc_beta_points_at_0_and_85c);
    RUN_TEST(test_ntc_open_and_short_are_not_readings);
    RUN_TEST(test_ntc_out_of_range_is_invalid);
    RUN_TEST(test_fan_rpm_and_health);
    RUN_TEST(test_collect_keeps_each_driver_to_its_own_modalities);
    return UNITY_END();
}
