/* Host tests for components/ota (Unity): versions, accepting a command,
 * rebooting when quiet, confirm or roll back, warm restarts, fw_update bytes. */
#include "unity.h"

#include "fx_entry.h"
#include "fx_ota.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define SHA "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
#define PARTITION 0x1E0000u

static fx_ota_node_t node;
static fx_ota_offer_t offer;

static void normal_node(void)
{
    memset(&node, 0, sizeof(node));
    node.have_fusion = true;
    node.fusion = FX_STATE_NORMAL;
    node.running_version = "0.4.0-m3";
    offer.version = "0.5.0-m5";
    offer.size = 1200000u;
    offer.sha256 = SHA;
    offer.url = "https://192.168.1.10:8443/fw/" SHA ".bin";
}

/* --- versions ---------------------------------------------------------------- */

static void test_versions_order_like_semver(void)
{
    TEST_ASSERT_TRUE(fx_ota_version_compare("0.5.0", "0.4.9") > 0);
    TEST_ASSERT_TRUE(fx_ota_version_compare("0.10.0", "0.9.0") > 0);
    TEST_ASSERT_TRUE(fx_ota_version_compare("0.5.0-m5", "0.5.0") < 0); /* a release follows its prereleases */
    TEST_ASSERT_TRUE(fx_ota_version_compare("0.5.0-rc1", "0.5.0-rc2") < 0);
    TEST_ASSERT_EQUAL_INT(0, fx_ota_version_compare("v1.2.3", "1.2.3"));
    TEST_ASSERT_EQUAL_INT(0, fx_ota_version_compare("1.2.3+inject", "1.2.3"));
    TEST_ASSERT_TRUE(fx_ota_version_compare("1.2", "1.2.1") < 0);
}

static void test_sha256_hex(void)
{
    uint8_t out[32];
    TEST_ASSERT_TRUE(fx_ota_parse_sha256(SHA, out));
    TEST_ASSERT_EQUAL_HEX8(0x1f, out[31]);
    TEST_ASSERT_FALSE(fx_ota_parse_sha256("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", out));
    TEST_ASSERT_FALSE(fx_ota_parse_sha256("0001", out));
    TEST_ASSERT_FALSE(fx_ota_parse_sha256(NULL, out));
}

/* --- accepting a command --------------------------------------------------------- */

static void test_a_good_offer_in_normal_is_accepted(void)
{
    normal_node();
    TEST_ASSERT_NULL(fx_ota_refusal(&node, &offer, PARTITION));
    offer.version = "0.3.9"; /* going back is an operator's decision, not a refusal */
    TEST_ASSERT_NULL(fx_ota_refusal(&node, &offer, PARTITION));
}

static void test_the_command_itself_must_make_sense(void)
{
    normal_node();
    offer.version = "";
    TEST_ASSERT_EQUAL_STRING("the command's version is not valid", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    offer.version = "0.5.0 m5";
    TEST_ASSERT_NOT_NULL(fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    offer.sha256 = "abc";
    TEST_ASSERT_EQUAL_STRING("the command's sha256 is not 64 lowercase hex digits", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    offer.url = "http://192.168.1.10:8443/fw/x.bin";
    TEST_ASSERT_EQUAL_STRING("the command's url is not an https URL", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    offer.size = 0;
    TEST_ASSERT_EQUAL_STRING("the command gives no image size", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    offer.size = PARTITION + 1u;
    TEST_ASSERT_EQUAL_STRING("the image is larger than the update partition", fx_ota_refusal(&node, &offer, PARTITION));
}

static void test_only_a_quiet_confirmed_node_takes_an_update(void)
{
    normal_node();
    node.update_in_progress = true;
    TEST_ASSERT_EQUAL_STRING("an update is already in progress", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.pending_verify = true;
    TEST_ASSERT_EQUAL_STRING("the running image has not confirmed itself yet", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    offer.version = "0.4.0-m3";
    TEST_ASSERT_EQUAL_STRING("that version is already running", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.fusion = FX_STATE_WARMUP;
    TEST_ASSERT_EQUAL_STRING("the node is still warming up", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.have_fusion = false;
    TEST_ASSERT_EQUAL_STRING("the node is still warming up", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.fusion = FX_STATE_ELEVATED;
    TEST_ASSERT_EQUAL_STRING("the node is not in NORMAL", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.fusion = FX_STATE_CRITICAL;
    TEST_ASSERT_EQUAL_STRING("the node is not in NORMAL", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.self_test_running = true;
    TEST_ASSERT_EQUAL_STRING("a self-test is running", fx_ota_refusal(&node, &offer, PARTITION));
    normal_node();
    node.injecting = true;
    TEST_ASSERT_EQUAL_STRING("a flame injection is running", fx_ota_refusal(&node, &offer, PARTITION));
}

/* --- rebooting when quiet --------------------------------------------------------- */

static bool quiet_at(fx_ota_quiet_t *q, fx_state_t fusion, bool alarm_active, bool busy, uint32_t now_ms)
{
    const fx_ota_quiet_input_t in = {true, fusion, alarm_active, busy, now_ms};
    return fx_ota_quiet_step(q, &in, FX_OTA_QUIET_MS);
}

static void test_reboot_waits_for_a_whole_quiet_minute(void)
{
    fx_ota_quiet_t q;
    fx_ota_quiet_init(&q);
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_NORMAL, false, false, 1000));
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_NORMAL, false, false, 60999));
    TEST_ASSERT_TRUE(quiet_at(&q, FX_STATE_NORMAL, false, false, 61000));

    fx_ota_quiet_init(&q);
    quiet_at(&q, FX_STATE_NORMAL, false, false, 0);
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_ELEVATED, false, false, 50000)); /* a break starts the minute again */
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_NORMAL, false, false, 51000));
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_NORMAL, false, false, 110000));
    TEST_ASSERT_TRUE(quiet_at(&q, FX_STATE_NORMAL, false, false, 111000));
}

static void test_a_silenced_alarm_or_a_self_test_is_not_quiet(void)
{
    fx_ota_quiet_t q;
    fx_ota_quiet_init(&q);
    quiet_at(&q, FX_STATE_NORMAL, false, false, 0);
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_NORMAL, true, false, 70000));
    TEST_ASSERT_FALSE(quiet_at(&q, FX_STATE_NORMAL, false, true, 140000));
    const fx_ota_quiet_input_t no_fusion = {false, FX_STATE_NORMAL, false, false, 300000};
    TEST_ASSERT_FALSE(fx_ota_quiet_step(&q, &no_fusion, FX_OTA_QUIET_MS));
}

/* --- confirming a new image --------------------------------------------------------- */

static void test_a_healthy_image_confirms_before_the_deadline(void)
{
    const fx_ota_health_t h = {true, true, true, 0x0F, 0x0F, true};
    const char *missing = "x";
    TEST_ASSERT_EQUAL_INT(FX_OTA_CONFIRM, fx_ota_confirm_step(&h, 12000, FX_OTA_CONFIRM_MS, &missing));
    TEST_ASSERT_NULL(missing);
    const fx_ota_health_t more = {true, true, true, 0x0F, 0x07, true}; /* a sensor came back: still fine */
    TEST_ASSERT_EQUAL_INT(FX_OTA_CONFIRM, fx_ota_confirm_step(&more, 12000, FX_OTA_CONFIRM_MS, NULL));
}

static void test_an_unhealthy_image_waits_then_rolls_back(void)
{
    fx_ota_health_t h = {true, true, true, 0x0F, 0x0F, false};
    const char *missing = NULL;
    TEST_ASSERT_EQUAL_INT(FX_OTA_WAIT, fx_ota_confirm_step(&h, 30000, FX_OTA_CONFIRM_MS, &missing));
    TEST_ASSERT_EQUAL_STRING("not connected and synced to the gateway", missing);
    TEST_ASSERT_EQUAL_INT(FX_OTA_ROLLBACK, fx_ota_confirm_step(&h, 60000, FX_OTA_CONFIRM_MS, &missing));

    h.gateway_synced = true;
    h.sensors_ready = 0x07;
    TEST_ASSERT_EQUAL_INT(FX_OTA_ROLLBACK, fx_ota_confirm_step(&h, 61000, FX_OTA_CONFIRM_MS, &missing));
    TEST_ASSERT_EQUAL_STRING("a sensor that was ready on the previous image is not ready", missing);

    const fx_ota_health_t no_ledger = {false, true, true, 0x0F, 0x0F, true};
    fx_ota_confirm_step(&no_ledger, 1000, FX_OTA_CONFIRM_MS, &missing);
    TEST_ASSERT_EQUAL_STRING("the ledger has not stored this boot's entry", missing);
    const fx_ota_health_t no_alarm = {true, false, true, 0x0F, 0x0F, true};
    fx_ota_confirm_step(&no_alarm, 1000, FX_OTA_CONFIRM_MS, &missing);
    TEST_ASSERT_EQUAL_STRING("the alarm task is not running", missing);
}

/* --- warm restarts ------------------------------------------------------------------ */

static void test_warm_restarts_shorten_the_warm_up(void)
{
    TEST_ASSERT_TRUE(fx_ota_heaters_kept_power("software"));
    TEST_ASSERT_TRUE(fx_ota_heaters_kept_power("panic"));
    TEST_ASSERT_TRUE(fx_ota_heaters_kept_power("watchdog"));
    TEST_ASSERT_TRUE(fx_ota_heaters_kept_power("external"));
    TEST_ASSERT_FALSE(fx_ota_heaters_kept_power("power_on"));
    TEST_ASSERT_FALSE(fx_ota_heaters_kept_power("brownout"));
    TEST_ASSERT_FALSE(fx_ota_heaters_kept_power("other"));
    TEST_ASSERT_FALSE(fx_ota_heaters_kept_power(NULL));

    TEST_ASSERT_EQUAL_UINT32(30000u, fx_ota_warmup_ms(180, 30, true));
    TEST_ASSERT_EQUAL_UINT32(180000u, fx_ota_warmup_ms(180, 30, false));
    TEST_ASSERT_EQUAL_UINT32(20000u, fx_ota_warmup_ms(20, 30, true)); /* never longer than the cold warm-up */
}

/* --- the ledger entry ---------------------------------------------------------------- */

/* ledger/src/cbor.ts encode() of the same payloads. */
static const char *const GOLDEN_DOWNLOADED =
    "a565696d6167655820000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f6570686173656a646f776e6c6f6164"
    "656466726561736f6e606776657273696f6e68302e352e302d6d356c66726f6d5f76657273696f6e68302e342e302d6d33";
static const char *const GOLDEN_ROLLED_BACK =
    "a565696d616765582000000000000000000000000000000000000000000000000000000000000000006570686173656b726f6c6c65645f"
    "6261636b66726561736f6e6d6e6f7420636f6e6669726d65646776657273696f6e68302e352e302d6d356c66726f6d5f76657273696f6e"
    "68302e342e302d6d33";

static void assert_hex(const char *hex, const uint8_t *bytes, size_t len)
{
    char got[2 * FX_PAYLOAD_MAX + 1];
    TEST_ASSERT_EQUAL_UINT(strlen(hex) / 2u, len);
    fx_hex_encode(bytes, len, got);
    TEST_ASSERT_EQUAL_STRING(hex, got);
}

static void test_fw_update_payloads_match_reference(void)
{
    uint8_t out[FX_PAYLOAD_MAX];
    uint8_t image[32];
    TEST_ASSERT_TRUE(fx_ota_parse_sha256(SHA, image));
    assert_hex(GOLDEN_DOWNLOADED, out, fx_ev_fw_update(out, FX_OTA_DOWNLOADED, "0.5.0-m5", "0.4.0-m3", image, NULL));
    assert_hex(GOLDEN_ROLLED_BACK, out,
               fx_ev_fw_update(out, FX_OTA_ROLLED_BACK, "0.5.0-m5", "0.4.0-m3", NULL, "not confirmed"));
    TEST_ASSERT_EQUAL_STRING("failed", fx_ota_phase_name(FX_OTA_FAILED));
    TEST_ASSERT_EQUAL_STRING("confirmed", fx_ota_phase_name(FX_OTA_CONFIRMED));
    TEST_ASSERT_EQUAL_STRING("accepted", fx_ota_phase_name(FX_OTA_ACCEPTED));
}

static void test_an_image_of_another_kind_is_refused(void)
{
    fx_ota_image_kind_t k;
    memset(&k, 0, sizeof(k));
    memcpy(k.magic, FX_OTA_KIND_MAGIC, sizeof(k.magic));
    strcpy(k.kind, "evt-production");
    TEST_ASSERT_EQUAL_UINT(32u, sizeof(k));
    TEST_ASSERT_NULL(fx_ota_kind_refusal(&k, "evt-production"));
    /* a signed development image offered to a production unit */
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, "evt"));
    strcpy(k.kind, "evt");
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, "evt-production"));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, "devkit"));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, "evt-p"));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, NULL));

    /* an image from before M6 has app code, not the record, at that offset */
    memset(&k, 0xA5, sizeof(k));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, "devkit"));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(NULL, "devkit"));

    /* the magic, but a kind that is empty or not terminated */
    memset(&k, 0, sizeof(k));
    memcpy(k.magic, FX_OTA_KIND_MAGIC, sizeof(k.magic));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, ""));
    memset(k.kind, 'x', sizeof(k.kind));
    TEST_ASSERT_NOT_NULL(fx_ota_kind_refusal(&k, "devkit"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_versions_order_like_semver);
    RUN_TEST(test_sha256_hex);
    RUN_TEST(test_a_good_offer_in_normal_is_accepted);
    RUN_TEST(test_the_command_itself_must_make_sense);
    RUN_TEST(test_only_a_quiet_confirmed_node_takes_an_update);
    RUN_TEST(test_reboot_waits_for_a_whole_quiet_minute);
    RUN_TEST(test_a_silenced_alarm_or_a_self_test_is_not_quiet);
    RUN_TEST(test_a_healthy_image_confirms_before_the_deadline);
    RUN_TEST(test_an_unhealthy_image_waits_then_rolls_back);
    RUN_TEST(test_warm_restarts_shorten_the_warm_up);
    RUN_TEST(test_fw_update_payloads_match_reference);
    RUN_TEST(test_an_image_of_another_kind_is_refused);
    return UNITY_END();
}
