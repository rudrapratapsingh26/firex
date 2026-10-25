#include "fx_frame_rx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STAGE_LINE = 0, STAGE_BODY, STAGE_TRAILER };

static const char TRAILER[] = "\nFXEND\n";

void fx_frame_rx_init(fx_frame_rx_t *rx, fx_frame_sink_t sink, void *sink_ctx)
{
    memset(rx, 0, sizeof(*rx));
    rx->sink = sink;
    rx->sink_ctx = sink_ctx;
}

static fx_frame_status_t fail(fx_frame_rx_t *rx, const char *reason)
{
    snprintf(rx->error, sizeof(rx->error), "%s", reason);
    rx->stage = STAGE_LINE;
    rx->line_len = 0;
    return FX_FRAME_ERROR;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v > 0xFFFFFFFFul) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* Split the line in place on spaces. Returns the number of fields. */
static int split(char *line, char *fields[], int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        fields[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

static fx_frame_status_t handle_line(fx_frame_rx_t *rx)
{
    rx->line[rx->line_len] = '\0';
    rx->line_len = 0;
    if (strncmp(rx->line, "FX", 2) != 0) {
        rx->skipped_lines++;
        return FX_FRAME_PENDING;
    }

    char *f[8];
    const int n = split(rx->line, f, 8);
    if (n == 0) {
        return FX_FRAME_PENDING;
    }

    if (strcmp(f[0], "FXJPEG") == 0) {
        if (n != 6 || strcmp(f[1], "1") != 0 || !parse_u32(f[2], &rx->length) ||
            !parse_u32(f[4], &rx->width) || !parse_u32(f[5], &rx->height) || strlen(f[3]) != 64u) {
            return fail(rx, "malformed FXJPEG header");
        }
        if (rx->length == 0u || rx->length > FX_FRAME_MAX_BYTES) {
            return fail(rx, "frame length out of range");
        }
        for (int i = 0; i < 32; i++) {
            const int hi = hex_nibble(f[3][2 * i]), lo = hex_nibble(f[3][2 * i + 1]);
            if (hi < 0 || lo < 0) {
                return fail(rx, "malformed hash in FXJPEG header");
            }
            rx->expected[i] = (uint8_t)((hi << 4) | lo);
        }
        rx->received = 0;
        rx->trailer_at = 0;
        fx_sha256_init(&rx->sha);
        rx->stage = STAGE_BODY;
        return FX_FRAME_PENDING;
    }
    if (strcmp(f[0], "FXCHECK") == 0) {
        rx->check_ok = n == 5 && strcmp(f[1], "ok") == 0 && parse_u32(f[2], &rx->length) &&
                       parse_u32(f[3], &rx->width) && parse_u32(f[4], &rx->height) && rx->length > 0u;
        if (!rx->check_ok) {
            snprintf(rx->error, sizeof(rx->error), "%s", n >= 3 ? f[2] : "check failed");
        }
        return FX_FRAME_CHECK;
    }
    if (strcmp(f[0], "FXPONG") == 0) {
        return FX_FRAME_PONG;
    }
    if (strcmp(f[0], "FXERR") == 0) {
        return fail(rx, n >= 2 ? f[1] : "unspecified");
    }
    rx->skipped_lines++; /* FXREADY and anything newer this receiver does not know */
    return FX_FRAME_PENDING;
}

fx_frame_status_t fx_frame_rx_feed(fx_frame_rx_t *rx, const uint8_t *data, size_t len, size_t *used)
{
    size_t i = 0;
    fx_frame_status_t status = FX_FRAME_PENDING;

    while (i < len && status == FX_FRAME_PENDING) {
        if (rx->stage == STAGE_LINE) {
            const char c = (char)data[i++];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                status = handle_line(rx);
                continue;
            }
            if (rx->line_len < sizeof(rx->line) - 1u) {
                rx->line[rx->line_len++] = c;
            } else {
                rx->line_len = 0; /* an over-long line is noise, not a header */
                rx->skipped_lines++;
            }
        } else if (rx->stage == STAGE_BODY) {
            size_t take = rx->length - rx->received;
            if (take > len - i) {
                take = len - i;
            }
            fx_sha256_update(&rx->sha, data + i, take);
            if (rx->sink) {
                rx->sink(rx->sink_ctx, data + i, take);
            }
            rx->received += (uint32_t)take;
            i += take;
            if (rx->received == rx->length) {
                fx_sha256_final(&rx->sha, rx->actual);
                rx->stage = STAGE_TRAILER;
            }
        } else {
            if ((char)data[i] != TRAILER[rx->trailer_at]) {
                status = fail(rx, "missing FXEND trailer");
                i++;
                continue;
            }
            i++;
            if (++rx->trailer_at == sizeof(TRAILER) - 1u) {
                rx->stage = STAGE_LINE;
                status = memcmp(rx->actual, rx->expected, 32) == 0 ? FX_FRAME_JPEG
                                                                    : fail(rx, "sha256 mismatch");
            }
        }
    }
    if (used) {
        *used = i;
    }
    return status;
}
