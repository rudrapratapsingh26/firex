#include <string.h>

#include "fx_entry.h"

#define MAGIC 0x5846u /* "FX" read little-endian */
#define HEADER 4u
#define TRAILER 4u
#define BODY_FIXED (8u + 8u + 1u + 1u + 2u + 16u + 32u + 32u + 32u + 64u)

uint32_t fx_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_le(uint8_t *p, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get_le(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

size_t fx_record_encode(const fx_entry_t *e, uint8_t *out, size_t cap)
{
    size_t type_len = 0;
    while (type_len <= FX_TYPE_MAX && e->type[type_len] != 0) {
        type_len++;
    }
    if (type_len > FX_TYPE_MAX || e->payload_len > FX_PAYLOAD_MAX) {
        return 0;
    }
    const size_t body = BODY_FIXED + type_len + e->payload_len;
    const size_t total = HEADER + body + TRAILER;
    if (!out || cap < total || total > FX_RECORD_MAX) {
        return 0;
    }

    uint8_t *p = out;
    put_le(p, MAGIC, 2);
    put_le(p + 2, body, 2);
    p += HEADER;
    put_le(p, e->seq, 8);
    p += 8;
    put_le(p, e->ts, 8);
    p += 8;
    *p++ = (uint8_t)e->ts_src;
    *p++ = (uint8_t)type_len;
    memcpy(p, e->type, type_len);
    p += type_len;
    put_le(p, e->payload_len, 2);
    p += 2;
    memcpy(p, e->payload, e->payload_len);
    p += e->payload_len;
    memcpy(p, e->device_id, 16);
    p += 16;
    memcpy(p, e->fw_hash, 32);
    p += 32;
    memcpy(p, e->prev_hash, 32);
    p += 32;
    memcpy(p, e->hash, 32);
    p += 32;
    memcpy(p, e->sig, 64);
    p += 64;
    put_le(p, fx_crc32(0, out, (size_t)(p - out)), 4);
    return total;
}

size_t fx_record_size(const uint8_t header[4])
{
    if (get_le(header, 2) != MAGIC) {
        return 0;
    }
    const size_t body = (size_t)get_le(header + 2, 2);
    const size_t total = HEADER + body + TRAILER;
    if (body < BODY_FIXED || total > FX_RECORD_MAX) {
        return 0;
    }
    return total;
}

fx_rec_status_t fx_record_decode(const uint8_t *in, size_t avail, fx_entry_t *out, size_t *consumed)
{
    if (avail < HEADER) {
        return FX_REC_NEED_MORE;
    }
    if (get_le(in, 2) != MAGIC) {
        return FX_REC_BAD_MAGIC;
    }
    const size_t total = fx_record_size(in);
    if (total == 0) {
        return FX_REC_BAD_LENGTH;
    }
    if (avail < total) {
        return FX_REC_NEED_MORE;
    }
    if (fx_crc32(0, in, total - TRAILER) != (uint32_t)get_le(in + total - TRAILER, 4)) {
        return FX_REC_BAD_CRC;
    }

    const uint8_t *p = in + HEADER;
    const uint8_t *body_end = in + total - TRAILER;
    memset(out, 0, sizeof(*out));
    out->seq = get_le(p, 8);
    p += 8;
    out->ts = get_le(p, 8);
    p += 8;
    out->ts_src = (fx_ts_src_t)*p++;
    const size_t type_len = *p++;
    if (type_len > FX_TYPE_MAX || (size_t)(body_end - p) < type_len + 2u) {
        return FX_REC_BAD_LENGTH;
    }
    memcpy(out->type, p, type_len);
    out->type[type_len] = '\0';
    p += type_len;
    const size_t payload_len = (size_t)get_le(p, 2);
    p += 2;
    if (payload_len > FX_PAYLOAD_MAX || (size_t)(body_end - p) != payload_len + 16u + 32u * 3u + 64u) {
        return FX_REC_BAD_LENGTH;
    }
    out->payload_len = (uint16_t)payload_len;
    memcpy(out->payload, p, payload_len);
    p += payload_len;
    memcpy(out->device_id, p, 16);
    p += 16;
    memcpy(out->fw_hash, p, 32);
    p += 32;
    memcpy(out->prev_hash, p, 32);
    p += 32;
    memcpy(out->hash, p, 32);
    p += 32;
    memcpy(out->sig, p, 64);
    if (consumed) {
        *consumed = total;
    }
    return FX_REC_OK;
}
