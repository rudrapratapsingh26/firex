/*
 * Receiver for replies on the node <-> ESP32-CAM link (firmware/cam). Pure C.
 *
 *   FXJPEG 1 <length> <sha256 hex> <width> <height>\n <length bytes> \nFXEND\n
 *   FXCHECK ok <length> <width> <height>\n      a capture test, no image sent
 *   FXCHECK fail <reason>\n
 *   FXPONG ...\n
 *   FXERR <reason>\n
 *
 * Other lines (boot text) are skipped. JPEG bytes are handed to a sink as they
 * arrive and hashed by the node itself, so the hash that reaches the ledger is
 * one the node computed, then compared with the camera's.
 */
#ifndef FX_FRAME_RX_H
#define FX_FRAME_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_FRAME_MAX_BYTES 1048576u

typedef enum {
    FX_FRAME_PENDING = 0, /* more bytes needed */
    FX_FRAME_JPEG,        /* a whole JPEG arrived and both hashes agree */
    FX_FRAME_CHECK,       /* an FXCHECK reply: see check_ok */
    FX_FRAME_PONG,
    FX_FRAME_ERROR        /* FXERR, or a malformed or corrupted reply: see error */
} fx_frame_status_t;

typedef void (*fx_frame_sink_t)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    int stage;
    char line[128];
    size_t line_len;

    uint32_t length;
    uint32_t received;
    uint32_t width;
    uint32_t height;
    uint8_t expected[32];
    uint8_t actual[32];
    fx_sha256_ctx sha;
    uint8_t trailer_at;

    bool check_ok;
    char error[64];
    uint32_t skipped_lines;

    fx_frame_sink_t sink;
    void *sink_ctx;
} fx_frame_rx_t;

void fx_frame_rx_init(fx_frame_rx_t *rx, fx_frame_sink_t sink, void *sink_ctx);

/* Consume bytes until one reply completes or the input runs out. *used is how
 * many bytes were taken; anything after a completed reply is left for the next
 * call. After a non-PENDING status the receiver is ready for the next reply. */
fx_frame_status_t fx_frame_rx_feed(fx_frame_rx_t *rx, const uint8_t *data, size_t len, size_t *used);

#ifdef __cplusplus
}
#endif
#endif /* FX_FRAME_RX_H */
