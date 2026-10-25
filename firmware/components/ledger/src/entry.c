#include "fx_entry.h"

#include <string.h>

#include "fx_cbor.h"
#include "fx_sha256.h"

const char *fx_ts_src_name(fx_ts_src_t src)
{
    switch (src) {
    case FX_TS_NTP: return "ntp";
    case FX_TS_RTC: return "rtc";
    case FX_TS_GATEWAY: return "gateway";
    default: return "uncertain";
    }
}

size_t fx_entry_preimage(const fx_entry_t *e, uint8_t *out, size_t cap)
{
    if (!e || !out || cap < 32u || e->payload_len > FX_PAYLOAD_MAX) {
        return 0;
    }
    memcpy(out, e->prev_hash, 32);

    /* The body's eight keys, written in canonical order: sorted by encoded key
     * bytes, which for short text keys means by length, then bytewise. The host
     * tests check this order against fx_map_t. */
    fx_cbor_t w;
    fx_cbor_init(&w, out + 32, cap - 32u);
    fx_cbor_head(&w, 5, 8);
    fx_cbor_text(&w, "ts");
    fx_cbor_uint(&w, e->ts);
    fx_cbor_text(&w, "seq");
    fx_cbor_uint(&w, e->seq);
    fx_cbor_text(&w, "type");
    fx_cbor_text(&w, e->type);
    fx_cbor_text(&w, "ts_src");
    fx_cbor_text(&w, fx_ts_src_name(e->ts_src));
    fx_cbor_text(&w, "fw_hash");
    fx_cbor_bytes(&w, e->fw_hash, 32);
    fx_cbor_text(&w, "payload");
    fx_cbor_raw(&w, e->payload, e->payload_len);
    fx_cbor_text(&w, "device_id");
    fx_cbor_bytes(&w, e->device_id, 16);
    fx_cbor_text(&w, "prev_hash");
    fx_cbor_bytes(&w, e->prev_hash, 32);
    return w.overflow ? 0 : 32u + w.len;
}

bool fx_entry_compute_hash(const fx_entry_t *e, uint8_t hash[32])
{
    uint8_t pre[FX_PREIMAGE_MAX];
    const size_t n = fx_entry_preimage(e, pre, sizeof(pre));
    if (n == 0) {
        return false;
    }
    fx_sha256(pre, n, hash);
    return true;
}

/* --- identity -------------------------------------------------------------- */

static const uint8_t SPKI_P256_PREFIX[26] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00,
};

void fx_spki_p256(const uint8_t point[65], uint8_t spki[91])
{
    memcpy(spki, SPKI_P256_PREFIX, sizeof(SPKI_P256_PREFIX));
    memcpy(spki + sizeof(SPKI_P256_PREFIX), point, 65);
}

void fx_device_id(const uint8_t point[65], uint8_t id[16])
{
    uint8_t spki[91], digest[32];
    fx_spki_p256(point, spki);
    fx_sha256(spki, sizeof(spki), digest);
    memcpy(id, digest, 16);
}

size_t fx_spki_pem(const uint8_t point[65], char *out, size_t cap)
{
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char BEGIN[] = "-----BEGIN PUBLIC KEY-----\n";
    static const char END[] = "-----END PUBLIC KEY-----\n";
    uint8_t spki[91];
    fx_spki_p256(point, spki);

    char b64[128];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(spki); i += 3) {
        const uint32_t v = ((uint32_t)spki[i] << 16) |
                           (i + 1 < sizeof(spki) ? (uint32_t)spki[i + 1] << 8 : 0u) |
                           (i + 2 < sizeof(spki) ? (uint32_t)spki[i + 2] : 0u);
        b64[n++] = B64[(v >> 18) & 63u];
        b64[n++] = B64[(v >> 12) & 63u];
        b64[n++] = i + 1 < sizeof(spki) ? B64[(v >> 6) & 63u] : '=';
        b64[n++] = i + 2 < sizeof(spki) ? B64[v & 63u] : '=';
    }

    const size_t lines = (n + 63u) / 64u;
    const size_t total = sizeof(BEGIN) - 1u + n + lines + sizeof(END) - 1u;
    if (!out || cap < total + 1u) {
        return 0;
    }
    size_t at = 0;
    memcpy(out + at, BEGIN, sizeof(BEGIN) - 1u);
    at += sizeof(BEGIN) - 1u;
    for (size_t i = 0; i < n; i += 64u) {
        const size_t len = n - i < 64u ? n - i : 64u;
        memcpy(out + at, b64 + i, len);
        at += len;
        out[at++] = '\n';
    }
    memcpy(out + at, END, sizeof(END) - 1u);
    at += sizeof(END) - 1u;
    out[at] = '\0';
    return at;
}

/* --- JSON ------------------------------------------------------------------ */

void fx_hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = HEX[data[i] >> 4];
        out[2 * i + 1] = HEX[data[i] & 15u];
    }
    out[2 * len] = '\0';
}

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool overflow;
} json_t;

static void emit(json_t *j, const char *s, size_t n)
{
    if (j->overflow || n >= j->cap - j->len) {
        j->overflow = true;
        return;
    }
    memcpy(j->out + j->len, s, n);
    j->len += n;
    j->out[j->len] = '\0';
}

static void emit_str(json_t *j, const char *s)
{
    emit(j, s, strlen(s));
}

static void emit_u64(json_t *j, uint64_t v)
{
    char digits[21];
    int n = 0;
    do {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v);
    char text[21];
    for (int i = 0; i < n; i++) {
        text[i] = digits[n - 1 - i];
    }
    emit(j, text, (size_t)n);
}

static void emit_hex_string(json_t *j, const uint8_t *data, size_t len)
{
    char buf[2 * 64 + 1];
    emit(j, "\"", 1);
    for (size_t i = 0; i < len; i += 64u) {
        const size_t chunk = len - i < 64u ? len - i : 64u;
        fx_hex_encode(data + i, chunk, buf);
        emit(j, buf, 2 * chunk);
    }
    emit(j, "\"", 1);
}

static void emit_text_string(json_t *j, const uint8_t *s, size_t len)
{
    static const char HEX[] = "0123456789abcdef";
    emit(j, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = s[i];
        if (c == '"' || c == '\\') {
            const char esc[2] = {'\\', (char)c};
            emit(j, esc, 2);
        } else if (c < 0x20u) {
            const char esc[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15u]};
            emit(j, esc, 6);
        } else {
            emit(j, (const char *)&s[i], 1);
        }
    }
    emit(j, "\"", 1);
}

static bool read_head(const uint8_t **p, const uint8_t *end, uint8_t *major, uint8_t *info, uint64_t *value)
{
    if (*p >= end) {
        return false;
    }
    const uint8_t b = *(*p)++;
    *major = b >> 5;
    *info = b & 31u;
    if (*info < 24u) {
        *value = *info;
        return true;
    }
    size_t n = *info == 24u ? 1 : *info == 25u ? 2 : *info == 26u ? 4 : *info == 27u ? 8 : 0;
    if (n == 0 || (size_t)(end - *p) < n) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | *(*p)++;
    }
    *value = v;
    return true;
}

static bool item_to_json(json_t *j, const uint8_t **p, const uint8_t *end, int depth)
{
    uint8_t major, info;
    uint64_t v;
    if (depth > 6 || !read_head(p, end, &major, &info, &v)) {
        return false;
    }
    switch (major) {
    case 0:
        emit_u64(j, v);
        return true;
    case 2:
    case 3:
        if (v > (uint64_t)(end - *p)) {
            return false;
        }
        if (major == 2) {
            emit_hex_string(j, *p, (size_t)v);
        } else {
            emit_text_string(j, *p, (size_t)v);
        }
        *p += v;
        return true;
    case 4:
        emit(j, "[", 1);
        for (uint64_t i = 0; i < v; i++) {
            if (i) {
                emit(j, ",", 1);
            }
            if (!item_to_json(j, p, end, depth + 1)) {
                return false;
            }
        }
        emit(j, "]", 1);
        return true;
    case 5:
        emit(j, "{", 1);
        for (uint64_t i = 0; i < v; i++) {
            if (i) {
                emit(j, ",", 1);
            }
            if (*p >= end || (**p >> 5) != 3u) {
                return false; /* keys are text in every ledger map */
            }
            if (!item_to_json(j, p, end, depth + 1)) {
                return false;
            }
            emit(j, ":", 1);
            if (!item_to_json(j, p, end, depth + 1)) {
                return false;
            }
        }
        emit(j, "}", 1);
        return true;
    case 7:
        if (info == 20u) {
            emit_str(j, "false");
        } else if (info == 21u) {
            emit_str(j, "true");
        } else if (info == 22u) {
            emit_str(j, "null");
        } else {
            return false;
        }
        return true;
    default:
        return false; /* negative integers, tags: never in a ledger entry */
    }
}

size_t fx_cbor_to_json(const uint8_t *cbor, size_t len, char *out, size_t cap)
{
    if (!cbor || !out || cap == 0) {
        return 0;
    }
    json_t j = {out, cap, 0, false};
    out[0] = '\0';
    const uint8_t *p = cbor;
    const uint8_t *end = cbor + len;
    if (!item_to_json(&j, &p, end, 0) || p != end || j.overflow) {
        return 0;
    }
    return j.len;
}

size_t fx_entry_to_json(const fx_entry_t *e, char *out, size_t cap)
{
    if (!e || !out || cap == 0) {
        return 0;
    }
    json_t j = {out, cap, 0, false};
    out[0] = '\0';
    emit_str(&j, "{\"seq\":");
    emit_u64(&j, e->seq);
    emit_str(&j, ",\"ts\":");
    emit_u64(&j, e->ts);
    emit_str(&j, ",\"ts_src\":\"");
    emit_str(&j, fx_ts_src_name(e->ts_src));
    emit_str(&j, "\",\"device_id\":");
    emit_hex_string(&j, e->device_id, 16);
    emit_str(&j, ",\"type\":");
    emit_text_string(&j, (const uint8_t *)e->type, strlen(e->type));
    emit_str(&j, ",\"payload\":");
    const uint8_t *p = e->payload;
    if (!item_to_json(&j, &p, e->payload + e->payload_len, 0) || p != e->payload + e->payload_len) {
        return 0;
    }
    emit_str(&j, ",\"fw_hash\":");
    emit_hex_string(&j, e->fw_hash, 32);
    emit_str(&j, ",\"prev_hash\":");
    emit_hex_string(&j, e->prev_hash, 32);
    emit_str(&j, ",\"hash\":");
    emit_hex_string(&j, e->hash, 32);
    emit_str(&j, ",\"sig\":");
    emit_hex_string(&j, e->sig, 64);
    emit_str(&j, "}");
    return j.overflow ? 0 : j.len;
}
