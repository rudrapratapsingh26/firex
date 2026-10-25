/*
 * Host tests for components/net (Unity). Provisioning strings are compared with
 * firmware/test/provision_golden.h, generated from the gateway's own functions.
 */
#include "unity.h"

#include "fx_prov.h"
#include "fx_sha256.h"
#include "fx_sync.h"
#include "provision_golden.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* --- HMAC-SHA256, RFC 4231 ---------------------------------------------- */

static void test_hmac_rfc4231_cases(void)
{
    uint8_t key[20], mac[32];
    char out[65];
    memset(key, 0x0b, sizeof(key));
    fx_hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, mac);
    hex(mac, 32, out);
    TEST_ASSERT_EQUAL_STRING("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", out);

    const char *data = "what do ya want for nothing?";
    fx_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)data, strlen(data), mac);
    hex(mac, 32, out);
    TEST_ASSERT_EQUAL_STRING("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", out);

    /* Case 6: a key longer than the block is hashed first. */
    uint8_t long_key[131];
    memset(long_key, 0xaa, sizeof(long_key));
    const char *d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    fx_hmac_sha256(long_key, sizeof(long_key), (const uint8_t *)d6, strlen(d6), mac);
    hex(mac, 32, out);
    TEST_ASSERT_EQUAL_STRING("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", out);
}

/* --- pairing codes -------------------------------------------------------- */

static void test_pairing_code_matches_the_gateway(void)
{
    char code[11], formatted[12], norm[11];
    fx_pairing_code_from_random(GOLDEN_CODE_RANDOM, code);
    TEST_ASSERT_EQUAL_STRING(GOLDEN_CODE, code);
    fx_pairing_code_format(code, formatted);
    TEST_ASSERT_EQUAL_UINT('-', formatted[5]);
    TEST_ASSERT_TRUE(fx_pairing_code_normalise(formatted, norm));
    TEST_ASSERT_EQUAL_STRING(code, norm);
}

static void test_pairing_code_normalisation(void)
{
    char code[11];
    TEST_ASSERT_TRUE(fx_pairing_code_normalise(" abcde-fgh1o ", code));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGH10", code);
    TEST_ASSERT_TRUE(fx_pairing_code_normalise("ILOIL-01234", code));
    TEST_ASSERT_EQUAL_STRING("1101101234", code);
    TEST_ASSERT_FALSE(fx_pairing_code_normalise("ABCDE-FGHU0", code)); /* U is not in the alphabet */
    TEST_ASSERT_FALSE(fx_pairing_code_normalise("ABCD", code));
    TEST_ASSERT_FALSE(fx_pairing_code_normalise("ABCDEFGHJKM", code));
}

/* --- provisioning ------------------------------------------------------------ */

static fx_prov_ids_t golden_ids(void)
{
    fx_prov_ids_t ids;
    snprintf(ids.device_id, sizeof(ids.device_id), "%s", GOLDEN_DEVICE_ID);
    snprintf(ids.ledger_pub, sizeof(ids.ledger_pub), "%s", GOLDEN_LEDGER_PUB);
    snprintf(ids.tls_pub, sizeof(ids.tls_pub), "%s", GOLDEN_TLS_PUB);
    snprintf(ids.nonce, sizeof(ids.nonce), "%s", GOLDEN_NONCE);
    return ids;
}

static void test_message_and_proof_match_the_gateway(void)
{
    const fx_prov_ids_t ids = golden_ids();
    char message[400], proof[65];
    TEST_ASSERT_TRUE(fx_prov_message(&ids, message, sizeof(message)) > 0);
    TEST_ASSERT_EQUAL_STRING(GOLDEN_MESSAGE, message);
    fx_prov_proof(GOLDEN_CODE, &ids, proof);
    TEST_ASSERT_EQUAL_STRING(GOLDEN_PROOF, proof);
}

static void test_reply_mac_is_checked(void)
{
    fx_prov_reply_t reply = {GOLDEN_SITE_ID, GOLDEN_MQTT_HOST, GOLDEN_MQTT_PORT, GOLDEN_CA_PEM, GOLDEN_CERT_PEM};
    TEST_ASSERT_TRUE(fx_prov_reply_mac_ok(GOLDEN_CODE, GOLDEN_NONCE, &reply, GOLDEN_REPLY_MAC));

    TEST_ASSERT_FALSE(fx_prov_reply_mac_ok("ZZZZZZZZZZ", GOLDEN_NONCE, &reply, GOLDEN_REPLY_MAC));
    TEST_ASSERT_FALSE(fx_prov_reply_mac_ok(GOLDEN_CODE, "ffffffffffffffffffffffffffffffff", &reply, GOLDEN_REPLY_MAC));
    fx_prov_reply_t rogue = reply;
    rogue.ca_pem = "-----BEGIN CERTIFICATE-----\nMIIBrogue\n-----END CERTIFICATE-----\n";
    TEST_ASSERT_FALSE(fx_prov_reply_mac_ok(GOLDEN_CODE, GOLDEN_NONCE, &rogue, GOLDEN_REPLY_MAC));
    fx_prov_reply_t moved = reply;
    moved.mqtt_port = 1883;
    TEST_ASSERT_FALSE(fx_prov_reply_mac_ok(GOLDEN_CODE, GOLDEN_NONCE, &moved, GOLDEN_REPLY_MAC));
    TEST_ASSERT_FALSE(fx_prov_reply_mac_ok(GOLDEN_CODE, GOLDEN_NONCE, &reply, "short"));
}

static void test_request_json(void)
{
    const fx_prov_ids_t ids = golden_ids();
    char json[1024];
    const size_t n = fx_prov_request_json(&ids, "0.4.0-m3", "aa", "bb", GOLDEN_PROOF, json, sizeof(json));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"version\":1,\"device_id\":\"6d70642010849d0c483ec2af8e76f1ae\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"fw_version\":\"0.4.0-m3\""));
    TEST_ASSERT_EQUAL_UINT(0, fx_prov_request_json(&ids, "bad\"version", "aa", "bb", GOLDEN_PROOF, json, sizeof(json)));
    TEST_ASSERT_EQUAL_UINT(0, fx_prov_request_json(&ids, "0.4.0", "aa", "bb", GOLDEN_PROOF, json, 40));
}

/* --- gateway commands ---------------------------------------------------------- */

static void test_commands_parse(void)
{
    fx_cmd_t c;
    const char *sync = "{\"cmd\":\"sync\",\"have_seq\":-1}";
    TEST_ASSERT_TRUE(fx_cmd_parse(sync, strlen(sync), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_SYNC, c.kind);
    TEST_ASSERT_EQUAL_INT64(-1, c.number);

    const char *ack = "{ \"cmd\" : \"ack\", \"seq\" : 1234567 }";
    TEST_ASSERT_TRUE(fx_cmd_parse(ack, strlen(ack), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_ACK, c.kind);
    TEST_ASSERT_EQUAL_INT64(1234567, c.number);

    const char *resend = "{\"cmd\":\"resend\",\"from_seq\":42}";
    TEST_ASSERT_TRUE(fx_cmd_parse(resend, strlen(resend), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_RESEND, c.kind);

    char media[128];
    snprintf(media, sizeof(media), "{\"cmd\":\"media_ack\",\"sha256\":\"%s\"}", GOLDEN_PROOF);
    TEST_ASSERT_TRUE(fx_cmd_parse(media, strlen(media), &c));
    TEST_ASSERT_EQUAL_STRING(GOLDEN_PROOF, c.sha256);

    const char *future = "{\"cmd\":\"reboot\"}";
    TEST_ASSERT_TRUE(fx_cmd_parse(future, strlen(future), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_UNKNOWN, c.kind);
}

static void test_malformed_commands_are_refused(void)
{
    fx_cmd_t c;
    const char *bad[] = {
        "{\"cmd\":\"ack\"}",
        "{\"cmd\":\"ack\",\"seq\":\"7\"}",
        "{\"cmd\":\"ack\",\"seq\":-3}",
        "{\"cmd\":\"media_ack\",\"sha256\":\"abc\"}",
        "{\"seq\":7}",
        "not json",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(fx_cmd_parse(bad[i], strlen(bad[i]), &c), bad[i]);
    }
    /* the length bound is respected: the value lies past it */
    const char *cut = "{\"cmd\":\"ack\",\"seq\":99}";
    TEST_ASSERT_FALSE(fx_cmd_parse(cut, 16, &c));
}

static void test_m5_commands_parse(void)
{
    fx_cmd_t c;
    const char *time = "{\"cmd\":\"time\",\"unix_ms\":1790000000123}";
    TEST_ASSERT_TRUE(fx_cmd_parse(time, strlen(time), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_TIME, c.kind);
    TEST_ASSERT_EQUAL_INT64(1790000000123LL, c.number);

    char json[512];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"ota\",\"version\":\"0.6.0+m5\",\"size\":1263456,\"sha256\":\"%s\","
             "\"url\":\"https://192.168.1.10:8443/fw/%s.bin\"}",
             GOLDEN_PROOF, GOLDEN_PROOF);
    TEST_ASSERT_TRUE(fx_cmd_parse(json, strlen(json), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_OTA, c.kind);
    TEST_ASSERT_EQUAL_STRING("0.6.0+m5", c.version);
    TEST_ASSERT_EQUAL_UINT32(1263456u, c.size);
    TEST_ASSERT_EQUAL_STRING(GOLDEN_PROOF, c.sha256);
    TEST_ASSERT_EQUAL_UINT(0, strncmp(c.url, "https://192.168.1.10:8443/fw/", 29));

    snprintf(json, sizeof(json), "{\"cmd\":\"trace_ack\",\"sha256\":\"%s\"}", GOLDEN_PROOF);
    TEST_ASSERT_TRUE(fx_cmd_parse(json, strlen(json), &c));
    TEST_ASSERT_EQUAL_INT(FX_CMD_TRACE_ACK, c.kind);
    TEST_ASSERT_EQUAL_STRING(GOLDEN_PROOF, c.sha256);
}

static void test_malformed_m5_commands_are_refused(void)
{
    fx_cmd_t c;
    char json[512];
    const char *bad_time[] = {
        "{\"cmd\":\"time\"}",
        "{\"cmd\":\"time\",\"unix_ms\":0}",               /* a gateway that does not know the time */
        "{\"cmd\":\"time\",\"unix_ms\":\"1790000000123\"}",
    };
    for (size_t i = 0; i < sizeof(bad_time) / sizeof(bad_time[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(fx_cmd_parse(bad_time[i], strlen(bad_time[i]), &c), bad_time[i]);
    }
    const char *bad_ota[][3] = {
        /* version, size, url */
        {"0.6.0", "0", "https://gw:8443/fw/x.bin"},
        {"0.6 0", "100", "https://gw:8443/fw/x.bin"},
        {"", "100", "https://gw:8443/fw/x.bin"},
        {"0.6.0", "100", "ftp://gw/fw/x.bin"},
        {"0.6.0", "100", "https:///fw/x.bin"},
        {"0.6.0", "100", "https://gw:8443/fw/x y.bin"},
        {"0.6.0-a-version-string-too-long-for-it", "100", "https://gw:8443/fw/x.bin"},
    };
    for (size_t i = 0; i < sizeof(bad_ota) / sizeof(bad_ota[0]); i++) {
        snprintf(json, sizeof(json), "{\"cmd\":\"ota\",\"version\":\"%s\",\"size\":%s,\"sha256\":\"%s\",\"url\":\"%s\"}",
                 bad_ota[i][0], bad_ota[i][1], GOLDEN_PROOF, bad_ota[i][2]);
        TEST_ASSERT_FALSE_MESSAGE(fx_cmd_parse(json, strlen(json), &c), json);
    }
    snprintf(json, sizeof(json), "{\"cmd\":\"ota\",\"version\":\"0.6.0\",\"size\":100,\"url\":\"https://gw/fw/x.bin\"}");
    TEST_ASSERT_FALSE(fx_cmd_parse(json, strlen(json), &c)); /* no sha256 */
    const char *ack = "{\"cmd\":\"trace_ack\",\"sha256\":\"ABC\"}";
    TEST_ASSERT_FALSE(fx_cmd_parse(ack, strlen(ack), &c));
}

/* --- store and forward ------------------------------------------------------------ */

static fx_cmd_t cmd(fx_cmd_kind_t kind, int64_t n)
{
    fx_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.kind = kind;
    c.number = n;
    return c;
}

static void test_nothing_is_sent_before_sync(void)
{
    fx_sync_t s;
    fx_sync_init(&s, 8);
    TEST_ASSERT_EQUAL_INT64(-1, fx_sync_next(&s, 0, 20));
    fx_cmd_t c = cmd(FX_CMD_SYNC, -1);
    fx_sync_on_cmd(&s, &c, 0);
    TEST_ASSERT_EQUAL_INT64(0, fx_sync_next(&s, 0, 20));
}

static void test_window_limits_in_flight_entries(void)
{
    fx_sync_t s;
    fx_sync_init(&s, 3);
    fx_cmd_t c = cmd(FX_CMD_SYNC, 9);
    fx_sync_on_cmd(&s, &c, 0);
    int64_t sent = 0;
    for (int64_t n; (n = fx_sync_next(&s, 0, 100)) >= 0;) {
        fx_sync_sent(&s, n);
        sent++;
    }
    TEST_ASSERT_EQUAL_INT64(3, sent); /* 10, 11, 12 */
    c = cmd(FX_CMD_ACK, 11);
    fx_sync_on_cmd(&s, &c, 0);
    TEST_ASSERT_EQUAL_INT64(13, fx_sync_next(&s, 0, 100));
}

static void test_resend_rewinds_and_rotation_skips_forward(void)
{
    fx_sync_t s;
    fx_sync_init(&s, 8);
    fx_cmd_t c = cmd(FX_CMD_SYNC, 4);
    fx_sync_on_cmd(&s, &c, 0);
    for (int i = 0; i < 4; i++) fx_sync_sent(&s, fx_sync_next(&s, 0, 20));
    c = cmd(FX_CMD_RESEND, 6);
    fx_sync_on_cmd(&s, &c, 0);
    TEST_ASSERT_EQUAL_INT64(6, fx_sync_next(&s, 0, 20));

    /* The gateway holds 4, but the node's oldest entry is now 15: send from 15. */
    fx_sync_disconnected(&s);
    TEST_ASSERT_EQUAL_INT64(-1, fx_sync_next(&s, 15, 30));
    c = cmd(FX_CMD_SYNC, 4);
    fx_sync_on_cmd(&s, &c, 15);
    TEST_ASSERT_EQUAL_INT64(-1, fx_sync_next(&s, 15, 30)); /* 15 is beyond acked + window */
    fx_sync_init(&s, 16);
    fx_sync_on_cmd(&s, &c, 15);
    TEST_ASSERT_EQUAL_INT64(15, fx_sync_next(&s, 15, 30));
}

static void test_caught_up_sends_nothing(void)
{
    fx_sync_t s;
    fx_sync_init(&s, 8);
    fx_cmd_t c = cmd(FX_CMD_SYNC, 20);
    fx_sync_on_cmd(&s, &c, 0);
    TEST_ASSERT_EQUAL_INT64(-1, fx_sync_next(&s, 0, 20));
    TEST_ASSERT_EQUAL_INT64(-1, fx_sync_next(&s, 0, -1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hmac_rfc4231_cases);
    RUN_TEST(test_pairing_code_matches_the_gateway);
    RUN_TEST(test_pairing_code_normalisation);
    RUN_TEST(test_message_and_proof_match_the_gateway);
    RUN_TEST(test_reply_mac_is_checked);
    RUN_TEST(test_request_json);
    RUN_TEST(test_commands_parse);
    RUN_TEST(test_malformed_commands_are_refused);
    RUN_TEST(test_m5_commands_parse);
    RUN_TEST(test_malformed_m5_commands_are_refused);
    RUN_TEST(test_nothing_is_sent_before_sync);
    RUN_TEST(test_window_limits_in_flight_entries);
    RUN_TEST(test_resend_rewinds_and_rotation_skips_forward);
    RUN_TEST(test_caught_up_sends_nothing);
    return UNITY_END();
}
