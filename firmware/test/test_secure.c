/*
 * Host tests for the secure element's pure half (components/secure/src): the lock CRC,
 * the configuration FireX writes, reading lock state off a configuration zone, and the
 * provisioning step for every state a chip can be found in. The golden vectors are the
 * ones tools/factory checks its Python mirror against.
 */
#include "unity.h"

#include "fx_se_config.h"
#include "se_config_golden.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_crc_matches_cryptoauthlib_on_known_packets(void)
{
    static const uint8_t info[] = {0x07, 0x30, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX16(0x5D03, fx_se_crc16(info, sizeof(info))); /* crc_le 03 5D */
    uint8_t ramp[128];
    for (unsigned i = 0; i < sizeof(ramp); i++) {
        ramp[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_HEX16(0x249F, fx_se_crc16(ramp, sizeof(ramp))); /* crc_le 9F 24 */
    TEST_ASSERT_EQUAL_HEX16(0x0000, fx_se_crc16(NULL, 5));
    TEST_ASSERT_EQUAL_HEX16(0x0000, fx_se_crc16(info, 0));
}

static void test_apply_changes_exactly_the_eight_firex_bytes(void)
{
    uint8_t before[FX_SE_CONFIG_SIZE], after[FX_SE_CONFIG_SIZE];
    for (unsigned i = 0; i < FX_SE_CONFIG_SIZE; i++) {
        before[i] = (uint8_t)(0xA5u ^ i);
    }
    memcpy(after, before, sizeof(after));
    fx_se_config_apply(after);
    static const unsigned firex[8] = {20, 21, 22, 23, 96, 97, 98, 99};
    for (unsigned i = 0; i < FX_SE_CONFIG_SIZE; i++) {
        bool ours = false;
        for (unsigned k = 0; k < 8; k++) {
            ours = ours || firex[k] == i;
        }
        if (!ours) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], after[i], "a byte FireX does not own changed");
        }
    }
    static const uint8_t slot[4] = {0x81, 0x20, 0x81, 0x20};
    static const uint8_t key[4] = {0x33, 0x00, 0x33, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(slot, after + 20, 4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(key, after + 96, 4);

    uint8_t twice[FX_SE_CONFIG_SIZE];
    memcpy(twice, after, sizeof(twice));
    fx_se_config_apply(twice);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(after, twice, FX_SE_CONFIG_SIZE);
}

static void test_view_reads_lock_bytes_and_slot_locked_bits(void)
{
    uint8_t cfg[FX_SE_CONFIG_SIZE];
    memcpy(cfg, GOLDEN_SE_FACTORY, sizeof(cfg));
    fx_se_config_view_t v;
    fx_se_config_view(cfg, &v);
    TEST_ASSERT_FALSE(v.config_locked);
    TEST_ASSERT_FALSE(v.data_locked);
    TEST_ASSERT_FALSE(v.ledger_slot_locked);
    TEST_ASSERT_FALSE(v.tls_slot_locked);
    TEST_ASSERT_FALSE(v.slots_as_required);

    fx_se_config_apply(cfg);
    cfg[FX_SE_LOCK_CONFIG_AT] = 0x00;
    cfg[FX_SE_LOCK_VALUE_AT] = 0x00;
    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFD;       /* bit 1 clear: slot 1 locked */
    cfg[FX_SE_SLOT_LOCKED_AT + 1u] = 0xFF;
    fx_se_config_view(cfg, &v);
    TEST_ASSERT_TRUE(v.config_locked);
    TEST_ASSERT_TRUE(v.data_locked);
    TEST_ASSERT_FALSE(v.ledger_slot_locked);
    TEST_ASSERT_TRUE(v.tls_slot_locked);
    TEST_ASSERT_TRUE(v.slots_as_required);

    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFF;
    cfg[FX_SE_SLOT_LOCKED_AT + 1u] = 0xFE;  /* slot 8: neither of ours */
    fx_se_config_view(cfg, &v);
    TEST_ASSERT_FALSE(v.ledger_slot_locked);
    TEST_ASSERT_FALSE(v.tls_slot_locked);

    cfg[FX_SE_LOCK_CONFIG_AT] = 0x12;       /* anything but 0x55 is locked */
    fx_se_config_view(cfg, &v);
    TEST_ASSERT_TRUE(v.config_locked);

    cfg[FX_SE_KEY_CONFIG_AT(1)] = 0x13;     /* slot 1 wrong: not as required */
    fx_se_config_view(cfg, &v);
    TEST_ASSERT_FALSE(v.slots_as_required);
}

static void step_is(fx_se_step_t expected, const uint8_t *cfg, bool refusal_has_reason)
{
    const char *why = "stale";
    const fx_se_step_t got = fx_se_next_step(cfg, &why);
    TEST_ASSERT_EQUAL_STRING(fx_se_step_name(expected), fx_se_step_name(got));
    if (refusal_has_reason) {
        TEST_ASSERT_NOT_NULL(why);
    } else {
        TEST_ASSERT_NULL(why);
    }
}

static void test_next_step_walks_a_blank_chip_to_done(void)
{
    uint8_t cfg[FX_SE_CONFIG_SIZE];
    memcpy(cfg, GOLDEN_SE_FACTORY, sizeof(cfg));
    step_is(FX_SE_STEP_WRITE_CONFIG, cfg, false);
    fx_se_config_apply(cfg);
    step_is(FX_SE_STEP_LOCK_CONFIG, cfg, false);
    cfg[FX_SE_LOCK_CONFIG_AT] = 0x00;
    step_is(FX_SE_STEP_LOCK_DATA, cfg, false);
    cfg[FX_SE_LOCK_VALUE_AT] = 0x00;
    step_is(FX_SE_STEP_GENKEY_LEDGER, cfg, false);
    /* Power lost after GenKey, before the slot lock: the zone reads the same, so generate again. */
    step_is(FX_SE_STEP_GENKEY_LEDGER, cfg, false);
    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFE;
    step_is(FX_SE_STEP_DONE, cfg, false);
    /* Slot 1 locked later changes nothing for the ledger key. */
    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFC;
    step_is(FX_SE_STEP_DONE, cfg, false);
}

static void test_next_step_refuses_chips_it_cannot_finish(void)
{
    uint8_t cfg[FX_SE_CONFIG_SIZE];

    memcpy(cfg, GOLDEN_SE_FACTORY, sizeof(cfg)); /* locked by someone else */
    cfg[FX_SE_LOCK_CONFIG_AT] = 0x00;
    step_is(FX_SE_STEP_REFUSE, cfg, true);
    cfg[FX_SE_LOCK_VALUE_AT] = 0x00;
    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFE;
    step_is(FX_SE_STEP_REFUSE, cfg, true);        /* even with a "locked key" in slot 0 */

    memcpy(cfg, GOLDEN_SE_APPLIED, sizeof(cfg));  /* impossible: data locked, config not */
    cfg[FX_SE_LOCK_VALUE_AT] = 0x00;
    step_is(FX_SE_STEP_REFUSE, cfg, true);

    memcpy(cfg, GOLDEN_SE_APPLIED, sizeof(cfg));  /* slot 0 lock bit already clear */
    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFE;
    step_is(FX_SE_STEP_REFUSE, cfg, true);

    memcpy(cfg, GOLDEN_SE_APPLIED, sizeof(cfg));  /* config locked, slot 0 locked, data not */
    cfg[FX_SE_LOCK_CONFIG_AT] = 0x00;
    cfg[FX_SE_SLOT_LOCKED_AT] = 0xFE;
    step_is(FX_SE_STEP_REFUSE, cfg, true);

    step_is(FX_SE_STEP_REFUSE, NULL, true);
    TEST_ASSERT_EQUAL_INT(FX_SE_STEP_REFUSE, fx_se_next_step(cfg, NULL)); /* why is optional */
}

static void test_step_names(void)
{
    TEST_ASSERT_EQUAL_STRING("write_config", fx_se_step_name(FX_SE_STEP_WRITE_CONFIG));
    TEST_ASSERT_EQUAL_STRING("lock_config", fx_se_step_name(FX_SE_STEP_LOCK_CONFIG));
    TEST_ASSERT_EQUAL_STRING("lock_data", fx_se_step_name(FX_SE_STEP_LOCK_DATA));
    TEST_ASSERT_EQUAL_STRING("genkey_ledger", fx_se_step_name(FX_SE_STEP_GENKEY_LEDGER));
    TEST_ASSERT_EQUAL_STRING("lock_ledger_slot", fx_se_step_name(FX_SE_STEP_LOCK_LEDGER_SLOT));
    TEST_ASSERT_EQUAL_STRING("done", fx_se_step_name(FX_SE_STEP_DONE));
    TEST_ASSERT_EQUAL_STRING("refuse", fx_se_step_name(FX_SE_STEP_REFUSE));
    TEST_ASSERT_EQUAL_STRING("refuse", fx_se_step_name((fx_se_step_t)42));
}

static void test_golden_vectors_match_the_code(void)
{
    static const uint8_t info[] = {0x07, 0x30, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX16(GOLDEN_SE_CRC_INFO, fx_se_crc16(info, sizeof(info)));
    uint8_t cfg[FX_SE_CONFIG_SIZE];
    for (unsigned i = 0; i < FX_SE_CONFIG_SIZE; i++) {
        cfg[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_HEX16(GOLDEN_SE_CRC_RAMP, fx_se_crc16(cfg, sizeof(cfg)));

    memcpy(cfg, GOLDEN_SE_FACTORY, sizeof(cfg));
    fx_se_config_apply(cfg);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_SE_APPLIED, cfg, FX_SE_CONFIG_SIZE);
    TEST_ASSERT_EQUAL_HEX16(GOLDEN_SE_APPLIED_CRC, fx_se_crc16(cfg, sizeof(cfg)));

    const size_t n = sizeof(GOLDEN_SE_CASES) / sizeof(GOLDEN_SE_CASES[0]);
    TEST_ASSERT_EQUAL_UINT(9, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(fx_se_step_name(GOLDEN_SE_CASES[i].step),
                                         fx_se_step_name(fx_se_next_step(GOLDEN_SE_CASES[i].config, NULL)),
                                         GOLDEN_SE_CASES[i].name);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_cryptoauthlib_on_known_packets);
    RUN_TEST(test_apply_changes_exactly_the_eight_firex_bytes);
    RUN_TEST(test_view_reads_lock_bytes_and_slot_locked_bits);
    RUN_TEST(test_next_step_walks_a_blank_chip_to_done);
    RUN_TEST(test_next_step_refuses_chips_it_cannot_finish);
    RUN_TEST(test_step_names);
    RUN_TEST(test_golden_vectors_match_the_code);
    return UNITY_END();
}
