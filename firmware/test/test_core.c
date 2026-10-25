/* Host tests for components/core (Unity): event payloads against the reference
 * encoder's bytes, settings, self-test judgements, crossings and time. */
#include "unity.h"

#include "fx_events.h"
#include "fx_selftest.h"
#include "fx_settings.h"
#include "ledger_golden.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint8_t out[FX_PAYLOAD_MAX];

static void assert_golden(size_t index, size_t len)
{
    TEST_ASSERT_EQUAL_UINT(GOLDEN_ENTRIES[index].payload_len, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_ENTRIES[index].payload, out, len);
}

static void test_hundredths_round_and_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, fx_hundredths(-0.2f));
    TEST_ASSERT_EQUAL_UINT32(21, fx_hundredths(0.21f));
    TEST_ASSERT_EQUAL_UINT32(68, fx_hundredths(0.675f));
    TEST_ASSERT_EQUAL_UINT32(100, fx_hundredths(1.4f));
    TEST_ASSERT_EQUAL_STRING("smoke", fx_modality_key(FX_SMOKE));
}

static void test_boot_self_test_fan_payloads_match_reference(void)
{
    assert_golden(0, fx_ev_boot(out, "0.1.0-m0", "power_on", 12));

    const fx_check_t checks[] = {
        {"camera", true}, {"fan", true}, {"heater", true}, {"buzzer", true}, {"flame_loopback", true},
    };
    assert_golden(1, fx_ev_self_test(out, true, checks, 5));
    assert_golden(2, fx_ev_fan_state(out, "on", 60, "current_ma", 95));
}

static void test_fusion_payloads_match_reference(void)
{
    assert_golden(3, fx_ev_threshold_cross(out, FX_GAS, 0.68f, true));

    const bool valid[FX_MODALITY_COUNT] = {true, true, true, true, true};
    const float elevated[FX_MODALITY_COUNT] = {0.68f, 0.21f, 0.0f, 0.12f, 0.44f};
    const fx_modality_t gas[] = {FX_GAS};
    assert_golden(4, fx_ev_state_change(out, FX_STATE_NORMAL, FX_STATE_ELEVATED, gas, 1, elevated, valid));

    const float critical[FX_MODALITY_COUNT] = {0.81f, 0.35f, 0.0f, 0.64f, 0.41f};
    const fx_modality_t gas_heat[] = {FX_GAS, FX_HEAT};
    assert_golden(6, fx_ev_state_change(out, FX_STATE_ELEVATED, FX_STATE_CRITICAL, gas_heat, 2, critical, valid));
}

static void test_media_payload_matches_reference(void)
{
    uint8_t frames[2][32];
    memcpy(frames[0], GOLDEN_MEDIA_0, 32);
    memcpy(frames[1], GOLDEN_MEDIA_1, 32);
    assert_golden(7, fx_ev_media_captured(out, (const uint8_t (*)[32])frames, 2, GOLDEN_MEDIA_2, "mjpeg", "640x480"));
}

static void test_state_change_leaves_out_invalid_modalities(void)
{
    const bool valid[FX_MODALITY_COUNT] = {true, false, true, true, true};
    const float scores[FX_MODALITY_COUNT] = {0.7f, 0.9f, 0.0f, 0.1f, 0.4f};
    const size_t n = fx_ev_state_change(out, FX_STATE_WARMUP, FX_STATE_NORMAL, NULL, 0, scores, valid);
    TEST_ASSERT_TRUE(n > 0);
    char json[512];
    TEST_ASSERT_TRUE(fx_cbor_to_json(out, n, json, sizeof(json)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"to\":\"NORMAL\",\"from\":\"WARMUP\",\"reason\":[],"
                             "\"scores\":{\"gas\":70,\"heat\":10,\"flame\":0,\"humidity\":40}}",
                             json);
}

static void test_other_payloads(void)
{
    char json[512];
    size_t n = fx_ev_media_captured(out, NULL, 0, NULL, "jpeg", "640x480");
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"clip\":null,\"codec\":\"jpeg\",\"frames\":[],\"trigger\":\"CRITICAL\",\"resolution\":\"640x480\"}", json);
    n = fx_ev_config_change(out, "fx_enter", 60, 65);
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"key\":\"fx_enter\",\"new\":65,\"old\":60}", json);
    n = fx_ev_silence(out, 300);
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"by\":\"button\",\"seconds\":300}", json);
    n = fx_ev_calibration(out, FX_GAS, 9015, "clean_air_median");
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"r0\":9015,\"method\":\"clean_air_median\",\"modality\":\"gas\"}", json);
    n = fx_ev_log_overwrite(out, 120, 243, false);
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"last_seq\":243,\"mirrored\":false,\"first_seq\":120}", json);
    n = fx_ev_fault(out, "smoke", true);
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"raised\",\"source\":\"smoke\"}", json);
    n = fx_ev_fan_state(out, "failed", 100, "rpm", 0);
    fx_cbor_to_json(out, n, json, sizeof(json));
    TEST_ASSERT_EQUAL_STRING("{\"rpm\":0,\"duty\":100,\"state\":\"failed\"}", json);
}

static void test_crossings(void)
{
    bool prev[FX_MODALITY_COUNT] = {false};
    bool active[FX_MODALITY_COUNT] = {true, false, false, false, true};
    bool valid[FX_MODALITY_COUNT] = {true, true, true, true, true};
    const float eff[FX_MODALITY_COUNT] = {0.65f, 0.1f, 0.0f, 0.2f, 0.9f};
    fx_crossing_t c[FX_MODALITY_COUNT];

    size_t n = fx_detect_crossings(prev, active, valid, eff, c);
    TEST_ASSERT_EQUAL_UINT(1, n); /* humidity never crosses */
    TEST_ASSERT_EQUAL_INT(FX_GAS, c[0].modality);
    TEST_ASSERT_TRUE(c[0].up);
    TEST_ASSERT_EQUAL_UINT(0, fx_detect_crossings(prev, active, valid, eff, c));

    valid[FX_GAS] = false; /* the sensor failed: not a falling score */
    active[FX_GAS] = false;
    TEST_ASSERT_EQUAL_UINT(0, fx_detect_crossings(prev, active, valid, eff, c));
    valid[FX_GAS] = true;
    active[FX_GAS] = false;
    TEST_ASSERT_EQUAL_UINT(0, fx_detect_crossings(prev, active, valid, eff, c));

    active[FX_GAS] = true;
    fx_detect_crossings(prev, active, valid, eff, c);
    active[FX_GAS] = false;
    n = fx_detect_crossings(prev, active, valid, eff, c);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_FALSE(c[0].up);
}

static void test_settings_defaults_are_consistent_and_match_the_engine(void)
{
    fx_settings_t s;
    fx_settings_defaults(&s);
    TEST_ASSERT_TRUE(fx_settings_consistent(&s, NULL));
    fx_config_t from_settings, engine;
    fx_settings_fusion(&s, &from_settings);
    fx_config_defaults(&engine);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, engine.enter, from_settings.enter);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, engine.gas_suppress, from_settings.gas_suppress);
    TEST_ASSERT_EQUAL_UINT32(engine.warmup_ms, from_settings.warmup_ms);
    TEST_ASSERT_TRUE(engine.mos_pair == from_settings.mos_pair);
    for (int i = 0; i < FX_SETTINGS_COUNT; i++) {
        TEST_ASSERT_TRUE(strlen(FX_SETTING_DEFS[i].key) <= 15); /* NVS key limit */
        TEST_ASSERT_TRUE(FX_SETTING_DEFS[i].def >= FX_SETTING_DEFS[i].min);
        TEST_ASSERT_TRUE(FX_SETTING_DEFS[i].def <= FX_SETTING_DEFS[i].max);
    }
}

static void test_settings_refuse_bad_changes_and_keep_the_old_value(void)
{
    fx_settings_t s;
    fx_settings_defaults(&s);
    uint32_t old = 0;
    const char *why = NULL;
    TEST_ASSERT_EQUAL_INT(FX_SET_UNKNOWN, fx_settings_set(&s, "nope", 1, &old, &why));
    TEST_ASSERT_EQUAL_INT(FX_SET_RANGE, fx_settings_set(&s, "fx_enter", 101, &old, &why));
    TEST_ASSERT_EQUAL_INT(FX_SET_INCONSISTENT, fx_settings_set(&s, "fx_exit", 60, &old, &why));
    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_EQUAL_INT(FX_SET_INCONSISTENT, fx_settings_set(&s, "fx_elev_ms", 1500, &old, &why));
    TEST_ASSERT_EQUAL_INT(FX_SET_INCONSISTENT, fx_settings_set(&s, "heat_full_c", 45, &old, &why));
    TEST_ASSERT_EQUAL_UINT32(40, s.v[FX_S_EXIT]);

    TEST_ASSERT_EQUAL_INT(FX_SET_OK, fx_settings_set(&s, "mq2_r0", 9015, &old, &why));
    TEST_ASSERT_EQUAL_UINT32(0, old);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 9015.0f, fx_settings_r0(&s, FX_GAS));

    TEST_ASSERT_EQUAL_INT(FX_SET_RANGE, fx_settings_set(&s, "fx_mos_pair", 2, &old, &why));
    TEST_ASSERT_EQUAL_INT(FX_SET_OK, fx_settings_set(&s, "fx_mos_pair", 0, &old, &why));
    TEST_ASSERT_EQUAL_UINT32(1, old);
    fx_config_t cfg;
    fx_settings_fusion(&s, &cfg);
    TEST_ASSERT_FALSE(cfg.mos_pair);

    TEST_ASSERT_EQUAL_UINT32(30, s.v[FX_S_WARM_S]);
    TEST_ASSERT_EQUAL_INT(FX_SET_RANGE, fx_settings_set(&s, "fx_warm_s", 3601, &old, &why));
    TEST_ASSERT_EQUAL_INT(FX_SET_OK, fx_settings_set(&s, "fx_warm_s", 0, &old, &why));
    TEST_ASSERT_EQUAL_UINT32(30, old);

    fx_heat_params_t heat;
    fx_flame_params_t flame;
    fx_mq_params_t mq2, mq135;
    fx_mq2_defaults(&mq2);
    fx_mq135_defaults(&mq135);
    fx_settings_heat(&s, &heat);
    fx_settings_flame(&s, &flame);
    fx_settings_mq(&s, &mq2, &mq135);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 57.0f, heat.temp_full_c);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 80.0f, flame.rms_full);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, mq135.ratio_full);
}

/* Built twice: plain, and with -DFX_CAM_FRAMES_LOCKED=3 as a production image is. */
static void test_settings_cam_frames_lock_follows_the_build(void)
{
    fx_settings_t s;
    fx_settings_defaults(&s);
    uint32_t old = 0;
    const char *why = NULL;
    const int id = fx_settings_find("cam_frames");
    TEST_ASSERT_EQUAL_INT(FX_S_CAM_FRAMES, id);
    TEST_ASSERT_EQUAL_UINT32(3, s.v[FX_S_CAM_FRAMES]);
    TEST_ASSERT_FALSE(fx_settings_locked(FX_S_ENTER));
#ifdef FX_CAM_FRAMES_LOCKED
    TEST_ASSERT_TRUE(fx_settings_locked(id));
    TEST_ASSERT_EQUAL_INT(FX_SET_LOCKED, fx_settings_set(&s, "cam_frames", 0, &old, &why));
    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_EQUAL_INT(FX_SET_LOCKED, fx_settings_set(&s, "cam_frames", 3, &old, &why));
    TEST_ASSERT_EQUAL_UINT32(3, s.v[FX_S_CAM_FRAMES]);
    /* a stored 5 from an earlier image is out of range, so loading ignores it */
    TEST_ASSERT_EQUAL_UINT32(3, FX_SETTING_DEFS[id].min);
    TEST_ASSERT_EQUAL_UINT32(3, FX_SETTING_DEFS[id].max);
    TEST_ASSERT_EQUAL_INT(FX_SET_OK, fx_settings_set(&s, "fx_enter", 70, &old, &why));
#else
    TEST_ASSERT_FALSE(fx_settings_locked(id));
    TEST_ASSERT_EQUAL_INT(FX_SET_OK, fx_settings_set(&s, "cam_frames", 0, &old, &why));
    TEST_ASSERT_EQUAL_UINT32(3, old);
    TEST_ASSERT_EQUAL_INT(FX_SET_RANGE, fx_settings_set(&s, "cam_frames", 6, &old, &why));
#endif
}

static void test_selftest_judgements(void)
{
    TEST_ASSERT_TRUE(fx_check_heater(true, 9000, 10000));
    TEST_ASSERT_FALSE(fx_check_heater(true, 9000, 0));      /* never calibrated */
    TEST_ASSERT_FALSE(fx_check_heater(true, 60000, 10000)); /* heater cold or open */
    TEST_ASSERT_FALSE(fx_check_heater(false, 9000, 10000));
    TEST_ASSERT_TRUE(fx_check_flame_loopback(0.05f, 0.7f));
    TEST_ASSERT_FALSE(fx_check_flame_loopback(0.05f, 0.2f));
    TEST_ASSERT_FALSE(fx_check_flame_loopback(0.5f, 0.9f)); /* already flickering: the test proves nothing */

    fx_selftest_report_t r;
    fx_selftest_begin(&r);
    TEST_ASSERT_FALSE(fx_selftest_pass(&r));
    fx_selftest_add(&r, "heater", true);
    fx_selftest_add(&r, "ledger", true);
    TEST_ASSERT_TRUE(fx_selftest_pass(&r));
    fx_selftest_add(&r, "camera", false);
    TEST_ASSERT_FALSE(fx_selftest_pass(&r));
    TEST_ASSERT_TRUE(fx_ev_self_test(out, fx_selftest_pass(&r), r.checks, r.n) > 0);
}

static void test_selftest_schedule(void)
{
    TEST_ASSERT_FALSE(fx_selftest_due(false, 0, 239999, 24, 240000));
    TEST_ASSERT_TRUE(fx_selftest_due(false, 0, 240000, 24, 240000));
    TEST_ASSERT_FALSE(fx_selftest_due(true, 240000, 240000 + 86399999u, 24, 240000));
    TEST_ASSERT_TRUE(fx_selftest_due(true, 240000, 240000 + 86400000u, 24, 240000));
}

static void test_clock_never_runs_backwards(void)
{
    fx_clock_t c;
    fx_ts_src_t src = FX_TS_NTP;
    fx_clock_init(&c, false, 0, 1789000000u);
    TEST_ASSERT_EQUAL_UINT64(1789000005u, fx_clock_now(&c, 5999, &src));
    TEST_ASSERT_EQUAL_INT(FX_TS_UNCERTAIN, src);
    fx_clock_init(&c, true, 1789500000u, 1789000000u); /* the log is newer than the build */
    TEST_ASSERT_EQUAL_UINT64(1789500001u, fx_clock_now(&c, 0, &src));
    fx_clock_init(&c, true, 1700000000u, 1789000000u); /* the build is newer than the log */
    TEST_ASSERT_EQUAL_UINT64(1789000000u, fx_clock_now(&c, 0, &src));
}

static void test_clock_first_gateway_sync_steps_forward_and_is_logged(void)
{
    fx_clock_t c;
    fx_ts_src_t src = FX_TS_UNCERTAIN;
    uint64_t step = 0;
    bool forward = false;
    fx_clock_init(&c, true, 1789000100u, 1789000000u); /* uncertain: 1789000101 + uptime */
    TEST_ASSERT_TRUE(fx_clock_sync(&c, 10000u, 1790000000500ull, &step, &forward));
    TEST_ASSERT_TRUE(forward);
    TEST_ASSERT_EQUAL_UINT64(1790000000500ull - 1789000111000ull, step);
    TEST_ASSERT_EQUAL_UINT64(1790000002u, fx_clock_now(&c, 12000u, &src));
    TEST_ASSERT_EQUAL_INT(FX_TS_GATEWAY, src);

    const size_t n = fx_ev_clock_sync(out, step, forward);
    char json[128];
    TEST_ASSERT_TRUE(fx_cbor_to_json(out, n, json, sizeof(json)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"source\":\"gateway\",\"step_ms\":999889500,\"direction\":\"forward\"}", json);
}

static void test_clock_small_step_is_applied_without_a_log_entry(void)
{
    fx_clock_t c;
    uint64_t step = 0;
    bool forward = false;
    fx_clock_init(&c, false, 0, 1789000000u);
    fx_clock_sync(&c, 0u, 1790000000000ull, &step, &forward);
    /* an hour later the gateway says 800 ms more than the crystal counted */
    TEST_ASSERT_FALSE(fx_clock_sync(&c, 3600000u, 1790003600800ull, &step, &forward));
    TEST_ASSERT_EQUAL_UINT64(800u, step);
    TEST_ASSERT_TRUE(forward);
    TEST_ASSERT_EQUAL_UINT64(1790003601u, fx_clock_now(&c, 3600200u, NULL));
}

static void test_clock_back_step_holds_ts_until_real_time_passes(void)
{
    fx_clock_t c;
    fx_ts_src_t src = FX_TS_UNCERTAIN;
    uint64_t step = 0;
    bool forward = true;
    fx_clock_init(&c, false, 0, 1789000000u);
    fx_clock_sync(&c, 0u, 1789001000000ull, &step, &forward);
    TEST_ASSERT_EQUAL_UINT64(1789001010u, fx_clock_stamp(&c, 10000u, &src)); /* an entry was written */

    TEST_ASSERT_TRUE(fx_clock_sync(&c, 10000u, 1789000990000ull, &step, &forward)); /* 20 s back */
    TEST_ASSERT_FALSE(forward);
    TEST_ASSERT_EQUAL_UINT64(20000u, step);
    TEST_ASSERT_EQUAL_UINT64(1789001010u, fx_clock_stamp(&c, 12000u, &src)); /* held, never earlier */
    TEST_ASSERT_EQUAL_INT(FX_TS_GATEWAY, src);
    TEST_ASSERT_EQUAL_UINT64(1789001010u, fx_clock_stamp(&c, 30000u, &src));
    TEST_ASSERT_EQUAL_UINT64(1789001011u, fx_clock_stamp(&c, 31000u, &src)); /* real time has passed it */
}

static void test_clock_is_uncertain_again_after_a_reboot_until_the_next_sync(void)
{
    fx_clock_t c;
    fx_ts_src_t src = FX_TS_GATEWAY;
    /* the last entry on flash was written under gateway time */
    fx_clock_init(&c, true, 1790000500u, 1789000000u);
    TEST_ASSERT_EQUAL_UINT64(1790000504u, fx_clock_now(&c, 3000u, &src));
    TEST_ASSERT_EQUAL_INT(FX_TS_UNCERTAIN, src);
    TEST_ASSERT_TRUE(fx_clock_sync(&c, 3000u, 1790000600000ull, NULL, NULL)); /* first sync of the boot */
    TEST_ASSERT_EQUAL_UINT64(1790000600u, fx_clock_now(&c, 3000u, &src));
    TEST_ASSERT_EQUAL_INT(FX_TS_GATEWAY, src);
}

static void test_trace_captured_payload(void)
{
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) {
        sha[i] = (uint8_t)i;
    }
    const size_t n = fx_ev_trace_captured(out, FX_STATE_ELEVATED, sha, 842, 500, 600);
    char json[256];
    TEST_ASSERT_TRUE(fx_cbor_to_json(out, n, json, sizeof(json)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"trace\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
                             "\"samples\":842,\"trigger\":\"ELEVATED\",\"period_ms\":500,\"pre_samples\":600}",
                             json);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hundredths_round_and_clamp);
    RUN_TEST(test_boot_self_test_fan_payloads_match_reference);
    RUN_TEST(test_fusion_payloads_match_reference);
    RUN_TEST(test_media_payload_matches_reference);
    RUN_TEST(test_state_change_leaves_out_invalid_modalities);
    RUN_TEST(test_other_payloads);
    RUN_TEST(test_crossings);
    RUN_TEST(test_settings_defaults_are_consistent_and_match_the_engine);
    RUN_TEST(test_settings_refuse_bad_changes_and_keep_the_old_value);
    RUN_TEST(test_selftest_judgements);
    RUN_TEST(test_selftest_schedule);
    RUN_TEST(test_clock_never_runs_backwards);
    RUN_TEST(test_clock_first_gateway_sync_steps_forward_and_is_logged);
    RUN_TEST(test_clock_small_step_is_applied_without_a_log_entry);
    RUN_TEST(test_clock_back_step_holds_ts_until_real_time_passes);
    RUN_TEST(test_clock_is_uncertain_again_after_a_reboot_until_the_next_sync);
    RUN_TEST(test_trace_captured_payload);
    RUN_TEST(test_settings_cam_frames_lock_follows_the_build);
    return UNITY_END();
}
