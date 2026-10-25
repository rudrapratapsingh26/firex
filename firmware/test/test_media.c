/* Host tests for components/media (Unity). */
#include "unity.h"

#include "fx_frame_rx.h"
#include "fx_sha256.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint8_t jpeg[3000];
static uint8_t sunk[4000];
static size_t sunk_len;

static void sink(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    memcpy(sunk + sunk_len, data, len);
    sunk_len += len;
}

/* The bytes the camera sends for `jpeg`, optionally with one payload byte corrupted. */
static size_t wire(uint8_t *out, bool corrupt)
{
    uint8_t digest[32];
    fx_sha256(jpeg, sizeof(jpeg), digest);
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    int n = sprintf((char *)out, "boot noise\r\nFXREADY firex-cam 0.3.0-m2 camera=ok\nFXJPEG 1 %u %s 640 480\n",
                    (unsigned)sizeof(jpeg), hex);
    memcpy(out + n, jpeg, sizeof(jpeg));
    if (corrupt) {
        out[n + 100] ^= 0xFF;
    }
    n += (int)sizeof(jpeg);
    memcpy(out + n, "\nFXEND\n", 7);
    return (size_t)n + 7u;
}

static fx_frame_rx_t rx;
static uint8_t buf[8000];

static fx_frame_status_t feed_in_chunks(const uint8_t *data, size_t len, size_t chunk)
{
    fx_frame_status_t st = FX_FRAME_PENDING;
    size_t at = 0;
    while (at < len && st == FX_FRAME_PENDING) {
        size_t n = len - at < chunk ? len - at : chunk;
        size_t used = 0;
        st = fx_frame_rx_feed(&rx, data + at, n, &used);
        at += used;
    }
    return st;
}

static void fill_jpeg(void)
{
    for (size_t i = 0; i < sizeof(jpeg); i++) {
        jpeg[i] = (uint8_t)(i * 31u + 7u);
    }
    jpeg[0] = 0xFF;
    jpeg[1] = 0xD8;
    sunk_len = 0;
}

static void test_a_frame_in_odd_chunks_hashes_and_matches(void)
{
    fill_jpeg();
    const size_t n = wire(buf, false);
    const size_t chunks[] = {1, 7, 64, 1000, n};
    for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
        sunk_len = 0;
        fx_frame_rx_init(&rx, sink, NULL);
        TEST_ASSERT_EQUAL_INT(FX_FRAME_JPEG, feed_in_chunks(buf, n, chunks[k]));
        TEST_ASSERT_EQUAL_UINT(sizeof(jpeg), sunk_len);
        TEST_ASSERT_EQUAL_MEMORY(jpeg, sunk, sizeof(jpeg));
        TEST_ASSERT_EQUAL_UINT32(640, rx.width);
        TEST_ASSERT_EQUAL_UINT32(2, rx.skipped_lines);
    }
    uint8_t digest[32];
    fx_sha256(jpeg, sizeof(jpeg), digest);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(digest, rx.actual, 32);
}

static void test_a_corrupted_byte_is_a_hash_mismatch(void)
{
    fill_jpeg();
    const size_t n = wire(buf, true);
    fx_frame_rx_init(&rx, sink, NULL);
    TEST_ASSERT_EQUAL_INT(FX_FRAME_ERROR, feed_in_chunks(buf, n, 100));
    TEST_ASSERT_EQUAL_STRING("sha256 mismatch", rx.error);
}

static void test_missing_trailer(void)
{
    fill_jpeg();
    size_t n = wire(buf, false);
    buf[n - 3] = 'X';
    fx_frame_rx_init(&rx, sink, NULL);
    TEST_ASSERT_EQUAL_INT(FX_FRAME_ERROR, feed_in_chunks(buf, n, 50));
    TEST_ASSERT_EQUAL_STRING("missing FXEND trailer", rx.error);
}

static void test_camera_error_check_and_pong(void)
{
    const char *reply = "FXERR capture_failed\nFXCHECK ok 23456 640 480\nFXCHECK fail no_sensor\nFXPONG firex-cam\n";
    const size_t len = strlen(reply);
    size_t at = 0, used = 0;
    fx_frame_rx_init(&rx, NULL, NULL);

    TEST_ASSERT_EQUAL_INT(FX_FRAME_ERROR, fx_frame_rx_feed(&rx, (const uint8_t *)reply, len, &used));
    TEST_ASSERT_EQUAL_STRING("capture_failed", rx.error);
    at += used;
    TEST_ASSERT_EQUAL_INT(FX_FRAME_CHECK, fx_frame_rx_feed(&rx, (const uint8_t *)reply + at, len - at, &used));
    TEST_ASSERT_TRUE(rx.check_ok);
    TEST_ASSERT_EQUAL_UINT32(23456, rx.length);
    at += used;
    TEST_ASSERT_EQUAL_INT(FX_FRAME_CHECK, fx_frame_rx_feed(&rx, (const uint8_t *)reply + at, len - at, &used));
    TEST_ASSERT_FALSE(rx.check_ok);
    TEST_ASSERT_EQUAL_STRING("no_sensor", rx.error);
    at += used;
    TEST_ASSERT_EQUAL_INT(FX_FRAME_PONG, fx_frame_rx_feed(&rx, (const uint8_t *)reply + at, len - at, &used));
    TEST_ASSERT_EQUAL_UINT(len, at + used);
}

static void test_malformed_headers(void)
{
    const char *bad[] = {
        "FXJPEG 1 0 0000000000000000000000000000000000000000000000000000000000000000 640 480\n",
        "FXJPEG 1 100 nothex 640 480\n",
        "FXJPEG 2 100 0000000000000000000000000000000000000000000000000000000000000000 640 480\n",
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        fx_frame_rx_init(&rx, NULL, NULL);
        size_t used = 0;
        TEST_ASSERT_EQUAL_INT(FX_FRAME_ERROR, fx_frame_rx_feed(&rx, (const uint8_t *)bad[k], strlen(bad[k]), &used));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_frame_in_odd_chunks_hashes_and_matches);
    RUN_TEST(test_a_corrupted_byte_is_a_hash_mismatch);
    RUN_TEST(test_missing_trailer);
    RUN_TEST(test_camera_error_check_and_pong);
    RUN_TEST(test_malformed_headers);
    return UNITY_END();
}
