#include "fx_sync.h"

#include <stdlib.h>
#include <string.h>

/* Find "key" followed by optional spaces and a colon; return the first value character. */
static const char *find_value(const char *json, size_t len, const char *key)
{
    const size_t klen = strlen(key);
    for (size_t i = 0; i + klen + 2 < len; i++) {
        if (json[i] != '"' || strncmp(json + i + 1, key, klen) != 0 || json[i + 1 + klen] != '"') {
            continue;
        }
        size_t j = i + klen + 2;
        while (j < len && (json[j] == ' ' || json[j] == '\t')) j++;
        if (j >= len || json[j] != ':') continue;
        j++;
        while (j < len && (json[j] == ' ' || json[j] == '\t')) j++;
        return j < len ? json + j : NULL;
    }
    return NULL;
}

static bool read_int(const char *json, size_t len, const char *key, int64_t *out)
{
    const char *v = find_value(json, len, key);
    if (!v) return false;
    const char *end = json + len;
    bool negative = false;
    if (v < end && *v == '-') {
        negative = true;
        v++;
    }
    if (v >= end || *v < '0' || *v > '9') return false;
    int64_t n = 0;
    while (v < end && *v >= '0' && *v <= '9') {
        if (n > (INT64_MAX - 9) / 10) return false;
        n = n * 10 + (*v++ - '0');
    }
    *out = negative ? -n : n;
    return true;
}

static bool read_string(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    const char *v = find_value(json, len, key);
    if (!v || *v != '"') return false;
    v++;
    size_t n = 0;
    const char *end = json + len;
    while (v < end && *v != '"') {
        if (*v == '\\' || n + 1 >= cap) return false;
        out[n++] = *v++;
    }
    if (v >= end) return false;
    out[n] = '\0';
    return true;
}

static bool read_sha256(const char *json, size_t len, char *out)
{
    if (!read_string(json, len, "sha256", out, 65) || strlen(out) != 64u) return false;
    for (size_t i = 0; i < 64u; i++) {
        const char c = out[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

#define MIN_UNIX_MS 1577836800000LL /* 2020-01-01: anything earlier is a gateway that does not know the time */

/* A firmware offer: every field present, a plain version string, and an http(s) URL with a host. */
static bool parse_ota(const char *json, size_t len, fx_cmd_t *out)
{
    int64_t size = 0;
    if (!read_sha256(json, len, out->sha256) || !read_int(json, len, "size", &size) || size <= 0 ||
        size > (int64_t)UINT32_MAX || !read_string(json, len, "version", out->version, sizeof(out->version)) ||
        !out->version[0] || !read_string(json, len, "url", out->url, sizeof(out->url))) {
        return false;
    }
    for (const char *p = out->version; *p; p++) {
        const bool ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        *p == '.' || *p == '+' || *p == '-' || *p == '_';
        if (!ok) return false;
    }
    size_t scheme = 0;
    if (strncmp(out->url, "https://", 8) == 0) {
        scheme = 8;
    } else if (strncmp(out->url, "http://", 7) == 0) {
        scheme = 7;
    } else {
        return false;
    }
    if (out->url[scheme] == '\0' || out->url[scheme] == '/') return false;
    for (const char *p = out->url; *p; p++) {
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) return false;
    }
    out->size = (uint32_t)size;
    return true;
}

bool fx_cmd_parse(const char *json, size_t len, fx_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    char cmd[16];
    if (!json || !read_string(json, len, "cmd", cmd, sizeof(cmd))) return false;

    if (strcmp(cmd, "time") == 0) {
        out->kind = FX_CMD_TIME;
        return read_int(json, len, "unix_ms", &out->number) && out->number >= MIN_UNIX_MS;
    }
    if (strcmp(cmd, "ota") == 0) {
        out->kind = FX_CMD_OTA;
        return parse_ota(json, len, out);
    }
    if (strcmp(cmd, "trace_ack") == 0) {
        out->kind = FX_CMD_TRACE_ACK;
        return read_sha256(json, len, out->sha256);
    }

    if (strcmp(cmd, "sync") == 0) {
        out->kind = FX_CMD_SYNC;
        return read_int(json, len, "have_seq", &out->number) && out->number >= -1;
    }
    if (strcmp(cmd, "ack") == 0) {
        out->kind = FX_CMD_ACK;
        return read_int(json, len, "seq", &out->number) && out->number >= 0;
    }
    if (strcmp(cmd, "resend") == 0) {
        out->kind = FX_CMD_RESEND;
        return read_int(json, len, "from_seq", &out->number) && out->number >= 0;
    }
    if (strcmp(cmd, "media_ack") == 0) {
        out->kind = FX_CMD_MEDIA_ACK;
        if (!read_string(json, len, "sha256", out->sha256, sizeof(out->sha256)) || strlen(out->sha256) != 64u) return false;
        for (size_t i = 0; i < 64u; i++) {
            const char c = out->sha256[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }
    out->kind = FX_CMD_UNKNOWN;
    return true; /* a newer gateway's command: ignored, not an error */
}

void fx_sync_init(fx_sync_t *s, uint32_t window)
{
    s->synced = false;
    s->acked = -1;
    s->next = 0;
    s->window = window ? window : 1u;
}

void fx_sync_disconnected(fx_sync_t *s)
{
    s->synced = false;
}

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

void fx_sync_on_cmd(fx_sync_t *s, const fx_cmd_t *cmd, int64_t oldest_seq)
{
    switch (cmd->kind) {
    case FX_CMD_SYNC:
        /* The gateway is the truth about what it holds: after a gateway restore
         * or a new epoch have_seq may be lower than anything acknowledged before. */
        s->acked = cmd->number;
        s->next = max64(cmd->number + 1, oldest_seq);
        s->synced = true;
        break;
    case FX_CMD_ACK:
        s->acked = max64(s->acked, cmd->number);
        if (s->next <= s->acked) {
            s->next = s->acked + 1;
        }
        break;
    case FX_CMD_RESEND:
        s->next = max64(cmd->number, oldest_seq);
        break;
    default:
        break;
    }
}

int64_t fx_sync_next(const fx_sync_t *s, int64_t oldest_seq, int64_t last_seq)
{
    if (!s->synced || last_seq < 0) return -1;
    const int64_t n = max64(s->next, oldest_seq);
    if (n > last_seq || n > s->acked + (int64_t)s->window) return -1;
    return n;
}

void fx_sync_sent(fx_sync_t *s, int64_t seq)
{
    s->next = seq + 1;
}
