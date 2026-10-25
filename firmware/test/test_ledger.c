/*
 * Host tests for components/ledger (Unity). The golden cases come from
 * ledger/vectors/01-valid-chain.json through ledger/src/golden-c.ts, so every
 * byte compared here was produced by the TypeScript reference implementation.
 */
#include "unity.h"

#include "fx_cbor.h"
#include "fx_entry.h"
#include "fx_ledger.h"
#include "fx_ring.h"
#include "fx_sha256.h"
#include "ledger_golden.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void hex_to_bytes(const char *hex, uint8_t *out)
{
    for (size_t i = 0; hex[2 * i]; i++) {
        char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
        out[i] = (uint8_t)strtoul(pair, NULL, 16);
    }
}

/* --- SHA-256 and CRC-32 ------------------------------------------------ */

static void assert_digest(const char *expected_hex, const uint8_t digest[32])
{
    uint8_t expected[32];
    hex_to_bytes(expected_hex, expected);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, digest, 32);
}

static void test_sha256_known_answers(void)
{
    uint8_t d[32];
    fx_sha256("", 0, d);
    assert_digest("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", d);
    fx_sha256("abc", 3, d);
    assert_digest("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", d);
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    fx_sha256(two_blocks, strlen(two_blocks), d);
    assert_digest("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", d);
}

static void test_sha256_streaming_matches_one_shot(void)
{
    uint8_t data[1000], a[32], b[32];
    for (int i = 0; i < 1000; i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    fx_sha256(data, sizeof(data), a);
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    size_t at = 0;
    for (size_t step = 1; at < sizeof(data); step = step * 2 + 1) {
        const size_t n = at + step > sizeof(data) ? sizeof(data) - at : step;
        fx_sha256_update(&ctx, data + at, n);
        at += n;
    }
    fx_sha256_final(&ctx, b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, b, 32);
}

static void test_crc32_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, fx_crc32(0, (const uint8_t *)"123456789", 9));
}

/* --- CBOR ---------------------------------------------------------------- */

static void test_cbor_shortest_heads(void)
{
    uint8_t buf[64];
    fx_cbor_t w;
    fx_cbor_init(&w, buf, sizeof(buf));
    fx_cbor_uint(&w, 23);
    fx_cbor_uint(&w, 24);
    fx_cbor_uint(&w, 256);
    fx_cbor_uint(&w, 65536);
    fx_cbor_uint(&w, 4294967296ull);
    const uint8_t expected[] = {0x17, 0x18, 0x18, 0x19, 0x01, 0x00, 0x1a, 0x00, 0x01, 0x00, 0x00,
                                0x1b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_FALSE(w.overflow);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), w.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, sizeof(expected));
}

static void test_cbor_overflow_is_sticky(void)
{
    uint8_t buf[3];
    fx_cbor_t w;
    fx_cbor_init(&w, buf, sizeof(buf));
    fx_cbor_text(&w, "hello");
    fx_cbor_uint(&w, 1);
    TEST_ASSERT_TRUE(w.overflow);
}

static void test_map_sorts_keys_canonically_and_rejects_duplicates(void)
{
    fx_map_t m;
    uint8_t out[64];
    fx_map_init(&m);
    fx_map_uint(&m, "bb", 1);
    fx_map_uint(&m, "a", 2);
    fx_map_uint(&m, "ab", 3);
    const size_t n = fx_map_finish(&m, out, sizeof(out));
    const uint8_t expected[] = {0xa3, 0x61, 'a', 0x02, 0x62, 'a', 'b', 0x03, 0x62, 'b', 'b', 0x01};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, n);

    fx_map_init(&m);
    fx_map_uint(&m, "a", 1);
    fx_map_uint(&m, "a", 2);
    TEST_ASSERT_EQUAL_UINT(0, fx_map_finish(&m, out, sizeof(out)));
}

static void assert_golden_payload(size_t index, const uint8_t *payload, size_t len)
{
    TEST_ASSERT_EQUAL_UINT(GOLDEN_ENTRIES[index].payload_len, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_ENTRIES[index].payload, payload, len);
}

static void test_payloads_match_the_reference_encoder(void)
{
    uint8_t out[FX_PAYLOAD_MAX];
    fx_map_t m;

    fx_map_init(&m); /* boot */
    fx_map_text(&m, "fw_version", "0.1.0-m0");
    fx_map_text(&m, "reset_reason", "power_on");
    fx_map_uint(&m, "boot_count", 12);
    assert_golden_payload(0, out, fx_map_finish(&m, out, sizeof(out)));

    fx_map_t checks; /* self_test with a nested map of booleans */
    uint8_t checks_buf[128];
    fx_map_init(&checks);
    fx_map_bool(&checks, "heater", true);
    fx_map_bool(&checks, "buzzer", true);
    fx_map_bool(&checks, "flame_loopback", true);
    fx_map_bool(&checks, "fan", true);
    fx_map_bool(&checks, "camera", true);
    const size_t checks_len = fx_map_finish(&checks, checks_buf, sizeof(checks_buf));
    fx_map_init(&m);
    fx_map_text(&m, "result", "pass");
    fx_map_raw(&m, "checks", checks_buf, checks_len);
    assert_golden_payload(1, out, fx_map_finish(&m, out, sizeof(out)));

    fx_map_init(&m); /* fan_state */
    fx_map_text(&m, "state", "on");
    fx_map_uint(&m, "duty", 60);
    fx_map_uint(&m, "current_ma", 95);
    assert_golden_payload(2, out, fx_map_finish(&m, out, sizeof(out)));

    uint8_t frames[80]; /* media_captured: an array of byte strings, a byte string, text */
    fx_cbor_t w;
    fx_cbor_init(&w, frames, sizeof(frames));
    fx_cbor_array(&w, 2);
    fx_cbor_bytes(&w, GOLDEN_MEDIA_0, 32);
    fx_cbor_bytes(&w, GOLDEN_MEDIA_1, 32);
    fx_map_init(&m);
    fx_map_text(&m, "trigger", "CRITICAL");
    fx_map_raw(&m, "frames", frames, w.len);
    fx_map_bytes(&m, "clip", GOLDEN_MEDIA_2, 32);
    fx_map_text(&m, "codec", "mjpeg");
    fx_map_text(&m, "resolution", "640x480");
    assert_golden_payload(7, out, fx_map_finish(&m, out, sizeof(out)));
}

/* --- entries ------------------------------------------------------------ */

static void golden_to_entry(size_t i, fx_entry_t *e)
{
    const golden_entry_t *g = &GOLDEN_ENTRIES[i];
    memset(e, 0, sizeof(*e));
    e->seq = g->seq;
    e->ts = g->ts;
    e->ts_src = (fx_ts_src_t)g->ts_src;
    strcpy(e->type, g->type);
    e->payload_len = g->payload_len;
    memcpy(e->payload, g->payload, g->payload_len);
    memcpy(e->device_id, g->device_id, 16);
    memcpy(e->fw_hash, g->fw_hash, 32);
    memcpy(e->prev_hash, g->prev_hash, 32);
    memcpy(e->hash, g->hash, 32);
    memcpy(e->sig, g->sig, 64);
}

static void test_every_golden_hash_recomputes(void)
{
    fx_entry_t e;
    uint8_t hash[32];
    for (size_t i = 0; i < GOLDEN_COUNT; i++) {
        golden_to_entry(i, &e);
        TEST_ASSERT_TRUE(fx_entry_compute_hash(&e, hash));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_ENTRIES[i].hash, hash, 32);
    }
}

static void test_body_key_order_is_canonical(void)
{
    /* Rebuild the body through fx_map_t, which sorts, and compare with the
     * fixed order in fx_entry_preimage. */
    fx_entry_t e;
    golden_to_entry(4, &e);
    uint8_t pre[FX_PREIMAGE_MAX], body[FX_PREIMAGE_MAX];
    const size_t n = fx_entry_preimage(&e, pre, sizeof(pre));

    fx_map_t m;
    fx_map_init(&m);
    fx_map_bytes(&m, "prev_hash", e.prev_hash, 32);
    fx_map_bytes(&m, "device_id", e.device_id, 16);
    fx_map_raw(&m, "payload", e.payload, e.payload_len);
    fx_map_bytes(&m, "fw_hash", e.fw_hash, 32);
    fx_map_text(&m, "ts_src", "ntp");
    fx_map_text(&m, "type", e.type);
    fx_map_uint(&m, "seq", e.seq);
    fx_map_uint(&m, "ts", e.ts);
    const size_t body_len = fx_map_finish(&m, body, sizeof(body));
    TEST_ASSERT_EQUAL_UINT(n - 32, body_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, pre + 32, body_len);
}

static void test_device_id_and_pem_from_the_public_key(void)
{
    uint8_t id[16];
    fx_device_id(GOLDEN_PUBKEY, id);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_ENTRIES[0].device_id, id, 16);
    char pem[256];
    TEST_ASSERT_TRUE(fx_spki_pem(GOLDEN_PUBKEY, pem, sizeof(pem)) > 0);
    TEST_ASSERT_EQUAL_STRING(GOLDEN_PEM, pem);
}

static void test_payload_json_matches_the_reference(void)
{
    char json[FX_ENTRY_JSON_MAX];
    for (size_t i = 0; i < GOLDEN_COUNT; i++) {
        TEST_ASSERT_TRUE(fx_cbor_to_json(GOLDEN_ENTRIES[i].payload, GOLDEN_ENTRIES[i].payload_len, json, sizeof(json)) > 0);
        TEST_ASSERT_EQUAL_STRING(GOLDEN_ENTRIES[i].payload_json, json);
    }
    fx_entry_t e;
    golden_to_entry(0, &e);
    TEST_ASSERT_TRUE(fx_entry_to_json(&e, json, sizeof(json)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"seq\":0,\"ts\":1789000000,\"ts_src\":\"ntp\""));
}

static void test_json_escapes_text(void)
{
    uint8_t cbor[32];
    fx_cbor_t w;
    fx_cbor_init(&w, cbor, sizeof(cbor));
    fx_cbor_text(&w, "a\"b\\c\n");
    char json[64];
    TEST_ASSERT_TRUE(fx_cbor_to_json(cbor, w.len, json, sizeof(json)) > 0);
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\u000a\"", json);
}

/* --- storage records ----------------------------------------------------- */

static void test_record_round_trip_and_damage(void)
{
    fx_entry_t e, back;
    golden_to_entry(7, &e);
    uint8_t rec[FX_RECORD_MAX];
    const size_t n = fx_record_encode(&e, rec, sizeof(rec));
    TEST_ASSERT_TRUE(n > 0);
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(FX_REC_OK, fx_record_decode(rec, n, &back, &consumed));
    TEST_ASSERT_EQUAL_UINT(n, consumed);
    TEST_ASSERT_EQUAL_MEMORY(&e, &back, sizeof(e));

    TEST_ASSERT_EQUAL_INT(FX_REC_NEED_MORE, fx_record_decode(rec, n - 1, &back, &consumed));
    rec[40] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(FX_REC_BAD_CRC, fx_record_decode(rec, n, &back, &consumed));
    rec[0] = 0;
    TEST_ASSERT_EQUAL_INT(FX_REC_BAD_MAGIC, fx_record_decode(rec, n, &back, &consumed));
}

static void test_gateway_time_source_round_trips(void)
{
    /* SPEC v0.5: a unit whose clock the gateway set writes ts_src "gateway". */
    fx_entry_t e, back;
    golden_to_entry(3, &e);
    e.ts_src = FX_TS_GATEWAY;
    uint8_t rec[FX_RECORD_MAX];
    const size_t n = fx_record_encode(&e, rec, sizeof(rec));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(FX_REC_OK, fx_record_decode(rec, n, &back, NULL));
    TEST_ASSERT_EQUAL_INT(FX_TS_GATEWAY, back.ts_src);

    char json[FX_ENTRY_JSON_MAX];
    TEST_ASSERT_TRUE(fx_entry_to_json(&back, json, sizeof(json)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"ts_src\":\"gateway\""));
    uint8_t pre[FX_PREIMAGE_MAX];
    const size_t len = fx_entry_preimage(&back, pre, sizeof(pre));
    TEST_ASSERT_TRUE(len > 0);
    bool found = false;
    for (size_t i = 0; i + 8 <= len; i++) {
        found = found || memcmp(pre + i, "\x67gateway", 8) == 0; /* CBOR text of length 7 */
    }
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("gateway", fx_ts_src_name(FX_TS_GATEWAY));
}

/* --- RAM store for the ring ---------------------------------------------- */

#define RAM_SEGS 64
#define RAM_SEG_CAP 16384

typedef struct {
    bool exists[RAM_SEGS];
    size_t len[RAM_SEGS];
    uint8_t data[RAM_SEGS][RAM_SEG_CAP];
    int fail_appends;
} ram_store_t;

static ram_store_t ram;

static int ram_append(void *ctx, uint32_t seg, const uint8_t *data, size_t len)
{
    ram_store_t *s = ctx;
    if (seg >= RAM_SEGS || s->len[seg] + len > RAM_SEG_CAP || s->fail_appends) {
        return -1;
    }
    s->exists[seg] = true;
    memcpy(s->data[seg] + s->len[seg], data, len);
    s->len[seg] += len;
    return 0;
}

static int ram_read(void *ctx, uint32_t seg, uint32_t offset, uint8_t *buf, size_t len, size_t *got)
{
    ram_store_t *s = ctx;
    if (seg >= RAM_SEGS || !s->exists[seg]) {
        return -1;
    }
    const size_t avail = offset < s->len[seg] ? s->len[seg] - offset : 0;
    *got = len < avail ? len : avail;
    memcpy(buf, s->data[seg] + offset, *got);
    return 0;
}

static long ram_size(void *ctx, uint32_t seg)
{
    ram_store_t *s = ctx;
    return (seg < RAM_SEGS && s->exists[seg]) ? (long)s->len[seg] : -1;
}

static int ram_remove(void *ctx, uint32_t seg)
{
    ram_store_t *s = ctx;
    if (seg >= RAM_SEGS || !s->exists[seg]) {
        return -1;
    }
    s->exists[seg] = false;
    s->len[seg] = 0;
    return 0;
}

static int ram_range(void *ctx, uint32_t *first, uint32_t *last)
{
    ram_store_t *s = ctx;
    int lo = -1, hi = -1;
    for (int i = 0; i < RAM_SEGS; i++) {
        if (s->exists[i]) {
            if (lo < 0) lo = i;
            hi = i;
        }
    }
    if (lo < 0) {
        return 1;
    }
    *first = (uint32_t)lo;
    *last = (uint32_t)hi;
    return 0;
}

static const fx_store_ops_t RAM_OPS = {ram_append, ram_read, ram_size, ram_remove, ram_range, &ram};

static fx_ring_t ring;
static fx_entry_t work;

static void reset_ram(void)
{
    memset(&ram, 0, sizeof(ram));
}

static void test_ring_stores_and_iterates_in_order(void)
{
    reset_ram();
    TEST_ASSERT_EQUAL_INT(0, fx_ring_open(&ring, &RAM_OPS, 1536, 8));
    TEST_ASSERT_TRUE(ring.empty);
    for (size_t i = 0; i < GOLDEN_COUNT; i++) {
        golden_to_entry(i, &work);
        TEST_ASSERT_EQUAL_INT(0, fx_ring_append(&ring, &work));
    }
    fx_ring_iter_t it;
    fx_ring_iter_begin(&ring, &it);
    size_t count = 0;
    while (fx_ring_iter_next(&it, &work) == 1) {
        TEST_ASSERT_EQUAL_UINT64(count, work.seq);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_ENTRIES[count].hash, work.hash, 32);
        count++;
    }
    TEST_ASSERT_EQUAL_UINT(GOLDEN_COUNT, count);
    TEST_ASSERT_TRUE(ring.last > ring.first); /* 1.5 KiB segments: the chain spans several */
}

static void test_ring_reopen_recovers_the_chain_head(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 4096, 8);
    for (size_t i = 0; i < 6; i++) {
        golden_to_entry(i, &work);
        fx_ring_append(&ring, &work);
    }
    TEST_ASSERT_EQUAL_INT(0, fx_ring_open(&ring, &RAM_OPS, 4096, 8));
    TEST_ASSERT_TRUE(ring.have_last);
    TEST_ASSERT_EQUAL_UINT64(5, ring.last_seq);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_ENTRIES[5].hash, ring.last_hash, 32);
    TEST_ASSERT_FALSE(ring.torn);
}

static void test_ring_rotation_drops_the_oldest_segment(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 1024, 3); /* roughly two records per segment */
    for (size_t i = 0; i < GOLDEN_COUNT; i++) {
        golden_to_entry(i, &work);
        TEST_ASSERT_EQUAL_INT(0, fx_ring_append(&ring, &work));
    }
    TEST_ASSERT_TRUE(ring.removed_segments > 0);
    TEST_ASSERT_EQUAL_UINT32(3, ring.last - ring.first + 1);

    fx_ring_iter_t it;
    fx_ring_iter_begin(&ring, &it);
    TEST_ASSERT_EQUAL_INT(1, fx_ring_iter_next(&it, &work));
    const uint64_t first_seq = work.seq;
    TEST_ASSERT_TRUE(first_seq > 0);
    uint64_t expect = first_seq + 1;
    while (fx_ring_iter_next(&it, &work) == 1) {
        TEST_ASSERT_EQUAL_UINT64(expect++, work.seq);
    }
    TEST_ASSERT_EQUAL_UINT64(GOLDEN_COUNT, expect);

    fx_chain_report_t rep = fx_ledger_check_chain(&ring, NULL, &work);
    TEST_ASSERT_TRUE(rep.ok); /* a window that starts after rotation is still a valid chain */

    /* rotation reports what it removed, so the node can log a log_overwrite entry */
    uint64_t first = 99, last = 0;
    TEST_ASSERT_TRUE(fx_ring_take_removed(&ring, &first, &last));
    TEST_ASSERT_EQUAL_UINT64(0, first);
    TEST_ASSERT_EQUAL_UINT64(first_seq - 1, last);
    TEST_ASSERT_FALSE(fx_ring_take_removed(&ring, &first, &last));
}

static void test_ring_torn_tail_starts_a_new_segment(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 8192, 8);
    for (size_t i = 0; i < 4; i++) {
        golden_to_entry(i, &work);
        fx_ring_append(&ring, &work);
    }
    /* Power lost mid-write: half of the next record reaches flash. */
    uint8_t rec[FX_RECORD_MAX];
    golden_to_entry(4, &work);
    const size_t n = fx_record_encode(&work, rec, sizeof(rec));
    ram_append(&ram, ring.last, rec, n / 2);

    TEST_ASSERT_EQUAL_INT(0, fx_ring_open(&ring, &RAM_OPS, 8192, 8));
    TEST_ASSERT_TRUE(ring.torn);
    TEST_ASSERT_EQUAL_UINT64(3, ring.last_seq);
    const uint32_t torn_segment = ring.last;

    for (size_t i = 4; i < 6; i++) {
        golden_to_entry(i, &work);
        TEST_ASSERT_EQUAL_INT(0, fx_ring_append(&ring, &work));
    }
    TEST_ASSERT_EQUAL_UINT32(torn_segment + 1, ring.last);

    fx_ring_iter_t it;
    fx_ring_iter_begin(&ring, &it);
    size_t count = 0;
    while (fx_ring_iter_next(&it, &work) == 1) {
        TEST_ASSERT_EQUAL_UINT64(count, work.seq);
        count++;
    }
    TEST_ASSERT_EQUAL_UINT(6, count);
    TEST_ASSERT_EQUAL_UINT32(1, it.skipped_segments);
}

/* --- the chain writer ----------------------------------------------------- */

/* A stand-in for ECDSA: the signature is the digest followed by its own hash.
 * Enough to prove the writer signs the entry hash and the checker verifies it. */
static int fake_sign(void *ctx, const uint8_t digest[32], uint8_t sig[64])
{
    (void)ctx;
    memcpy(sig, digest, 32);
    fx_sha256(digest, 32, sig + 32);
    return 0;
}

static bool fake_verify(void *ctx, const uint8_t digest[32], const uint8_t sig[64])
{
    uint8_t expect[64];
    fake_sign(ctx, digest, expect);
    return memcmp(expect, sig, 64) == 0;
}

static const fx_signer_t FAKE_SIGNER = {fake_sign, fake_verify, NULL};
static const uint8_t DEVICE_A[16] = {0xA0, 1, 2, 3};
static const uint8_t DEVICE_B[16] = {0xB0, 1, 2, 3};
static const uint8_t FW[32] = {0xF0};

static size_t boot_payload(uint8_t *out, uint32_t boot_count)
{
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "fw_version", "0.3.0-m2");
    fx_map_text(&m, "reset_reason", "power_on");
    fx_map_uint(&m, "boot_count", boot_count);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}

static void test_ledger_writes_a_chain_that_checks_and_survives_reboot(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 2048, 8);
    fx_ledger_t led;
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_OK, fx_ledger_init(&led, &ring, FAKE_SIGNER, DEVICE_A, FW));
    uint8_t payload[FX_PAYLOAD_MAX];
    for (uint32_t i = 0; i < 5; i++) {
        const size_t n = boot_payload(payload, i);
        TEST_ASSERT_EQUAL_INT(FX_LEDGER_OK,
                              fx_ledger_append(&led, "boot", payload, n, 1000u + i, FX_TS_UNCERTAIN, &work));
    }
    TEST_ASSERT_EQUAL_UINT64(4, work.seq);

    /* reboot: reopen the store and continue */
    fx_ring_open(&ring, &RAM_OPS, 2048, 8);
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_OK, fx_ledger_init(&led, &ring, FAKE_SIGNER, DEVICE_A, FW));
    const size_t n = boot_payload(payload, 5);
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_OK, fx_ledger_append(&led, "boot", payload, n, 900u, FX_TS_UNCERTAIN, &work));
    TEST_ASSERT_EQUAL_UINT64(5, work.seq);
    TEST_ASSERT_EQUAL_UINT64(1004, work.ts); /* never earlier than the entry before it */

    fx_chain_report_t rep = fx_ledger_check_chain(&ring, &FAKE_SIGNER, &work);
    TEST_ASSERT_TRUE(rep.ok);
    TEST_ASSERT_EQUAL_UINT32(6, rep.entries);
}

static void test_chain_check_finds_a_forged_record(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 65536, 4);
    fx_ledger_t led;
    fx_ledger_init(&led, &ring, FAKE_SIGNER, DEVICE_A, FW);
    uint8_t payload[FX_PAYLOAD_MAX];
    for (uint32_t i = 0; i < 4; i++) {
        const size_t n = boot_payload(payload, i);
        fx_ledger_append(&led, "boot", payload, n, 2000u + i, FX_TS_UNCERTAIN, &work);
    }

    /* Rewrite entry 2 on flash as a forger would: new payload, valid CRC, old hash. */
    fx_ring_iter_t it;
    fx_ring_iter_begin(&ring, &it);
    uint32_t offset = 0;
    fx_entry_t e;
    for (int i = 0; i < 2; i++) {
        fx_ring_iter_next(&it, &e);
    }
    offset = it.off;
    fx_ring_iter_next(&it, &e);
    e.payload_len = (uint16_t)boot_payload(e.payload, 99);
    uint8_t rec[FX_RECORD_MAX];
    const size_t n = fx_record_encode(&e, rec, sizeof(rec));
    memcpy(ram.data[0] + offset, rec, n);

    fx_chain_report_t rep = fx_ledger_check_chain(&ring, &FAKE_SIGNER, &work);
    TEST_ASSERT_FALSE(rep.ok);
    TEST_ASSERT_EQUAL_UINT64(2, rep.failed_seq);
    TEST_ASSERT_EQUAL_STRING("hash does not match the entry's contents", rep.reason);
}

static void test_ledger_blocks_a_different_device_key(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 8192, 4);
    fx_ledger_t led;
    fx_ledger_init(&led, &ring, FAKE_SIGNER, DEVICE_A, FW);
    uint8_t payload[FX_PAYLOAD_MAX];
    size_t n = boot_payload(payload, 1);
    fx_ledger_append(&led, "boot", payload, n, 1, FX_TS_UNCERTAIN, &work);

    fx_ring_open(&ring, &RAM_OPS, 8192, 4);
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_BLOCKED, fx_ledger_init(&led, &ring, FAKE_SIGNER, DEVICE_B, FW));
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_BLOCKED, fx_ledger_append(&led, "boot", payload, n, 2, FX_TS_UNCERTAIN, &work));
}

static void test_ledger_reports_store_failure(void)
{
    reset_ram();
    fx_ring_open(&ring, &RAM_OPS, 8192, 4);
    fx_ledger_t led;
    fx_ledger_init(&led, &ring, FAKE_SIGNER, DEVICE_A, FW);
    uint8_t payload[FX_PAYLOAD_MAX];
    const size_t n = boot_payload(payload, 1);
    ram.fail_appends = 1;
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_STORE_FAILED, fx_ledger_append(&led, "boot", payload, n, 1, FX_TS_UNCERTAIN, &work));
    ram.fail_appends = 0;
    TEST_ASSERT_EQUAL_INT(FX_LEDGER_OK, fx_ledger_append(&led, "boot", payload, n, 1, FX_TS_UNCERTAIN, &work));
    TEST_ASSERT_EQUAL_UINT64(0, work.seq); /* a failed write does not consume a sequence number */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha256_known_answers);
    RUN_TEST(test_sha256_streaming_matches_one_shot);
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_cbor_shortest_heads);
    RUN_TEST(test_cbor_overflow_is_sticky);
    RUN_TEST(test_map_sorts_keys_canonically_and_rejects_duplicates);
    RUN_TEST(test_payloads_match_the_reference_encoder);
    RUN_TEST(test_every_golden_hash_recomputes);
    RUN_TEST(test_body_key_order_is_canonical);
    RUN_TEST(test_device_id_and_pem_from_the_public_key);
    RUN_TEST(test_payload_json_matches_the_reference);
    RUN_TEST(test_json_escapes_text);
    RUN_TEST(test_record_round_trip_and_damage);
    RUN_TEST(test_ring_stores_and_iterates_in_order);
    RUN_TEST(test_ring_reopen_recovers_the_chain_head);
    RUN_TEST(test_ring_rotation_drops_the_oldest_segment);
    RUN_TEST(test_ring_torn_tail_starts_a_new_segment);
    RUN_TEST(test_ledger_writes_a_chain_that_checks_and_survives_reboot);
    RUN_TEST(test_chain_check_finds_a_forged_record);
    RUN_TEST(test_ledger_blocks_a_different_device_key);
    RUN_TEST(test_ledger_reports_store_failure);
    RUN_TEST(test_gateway_time_source_round_trips);
    return UNITY_END();
}
