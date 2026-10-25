/*
 * Ledger entries (SPEC.md sections 1 and 2): the body, its preimage and hash,
 * the device identity derived from the public key, the JSON transport form the
 * verifier reads, and the record format entries are stored in on flash.
 *
 * Pure C. The signature itself is produced outside, by whatever holds the
 * device key (hw/device_key.c on the node), over the entry hash.
 */
#ifndef FX_ENTRY_H
#define FX_ENTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_PAYLOAD_MAX 512u
#define FX_TYPE_MAX 23u
#define FX_PREIMAGE_MAX 800u
#define FX_RECORD_MAX 800u
#define FX_ENTRY_JSON_MAX 2600u

typedef enum {
    FX_TS_NTP = 0,
    FX_TS_RTC = 1,
    FX_TS_UNCERTAIN = 2,
    FX_TS_GATEWAY = 3 /* set from the site gateway over the authenticated MQTT link (SPEC v0.5) */
} fx_ts_src_t;

typedef struct {
    uint64_t seq;
    uint64_t ts;
    fx_ts_src_t ts_src;
    uint8_t device_id[16];
    char type[FX_TYPE_MAX + 1];
    uint16_t payload_len;
    uint8_t payload[FX_PAYLOAD_MAX]; /* canonical CBOR map */
    uint8_t fw_hash[32];
    uint8_t prev_hash[32];
    uint8_t hash[32];
    uint8_t sig[64]; /* raw r || s */
} fx_entry_t;

const char *fx_ts_src_name(fx_ts_src_t src);

/* prev_hash || CBOR(body without hash and sig). Returns length, 0 if it does not fit. */
size_t fx_entry_preimage(const fx_entry_t *e, uint8_t *out, size_t cap);

/* SHA-256 of the preimage. False if the entry cannot be encoded. */
bool fx_entry_compute_hash(const fx_entry_t *e, uint8_t hash[32]);

/* --- identity: an uncompressed P-256 point (0x04 || X || Y) ---------------- */

#define FX_P256_POINT_LEN 65u
#define FX_SPKI_P256_LEN 91u

void fx_spki_p256(const uint8_t point[65], uint8_t spki[91]);
/* First 16 bytes of SHA-256 over the SPKI DER, as ledger/src/entry.ts derives it. */
void fx_device_id(const uint8_t point[65], uint8_t id[16]);
/* "-----BEGIN PUBLIC KEY-----\n...\n-----END PUBLIC KEY-----\n". Returns length or 0. */
size_t fx_spki_pem(const uint8_t point[65], char *out, size_t cap);

/* --- JSON transport (EntryJson in ledger/src/entry.ts) -------------------- */

void fx_hex_encode(const uint8_t *data, size_t len, char *out); /* writes 2*len chars and a NUL */
/* Canonical CBOR to JSON: byte strings become lowercase hex strings. Returns length or 0. */
size_t fx_cbor_to_json(const uint8_t *cbor, size_t len, char *out, size_t cap);
size_t fx_entry_to_json(const fx_entry_t *e, char *out, size_t cap);

/* --- storage record --------------------------------------------------------
 *   u16 magic "FX" | u16 body length | body | u32 CRC-32 of everything before it
 *   body: u64 seq, u64 ts, u8 ts_src, u8 type length, type, u16 payload length,
 *         payload, device_id[16], fw_hash[32], prev_hash[32], hash[32], sig[64]
 * Little-endian. The CRC catches a record torn by power loss; the hash chain
 * catches everything else. */

typedef enum {
    FX_REC_OK = 0,
    FX_REC_NEED_MORE,  /* not enough bytes for a whole record */
    FX_REC_BAD_MAGIC,
    FX_REC_BAD_LENGTH,
    FX_REC_BAD_CRC
} fx_rec_status_t;

uint32_t fx_crc32(uint32_t crc, const uint8_t *data, size_t len);
size_t fx_record_encode(const fx_entry_t *e, uint8_t *out, size_t cap);
/* Total record size announced by a 4-byte header, or 0 if the header is invalid. */
size_t fx_record_size(const uint8_t header[4]);
fx_rec_status_t fx_record_decode(const uint8_t *in, size_t avail, fx_entry_t *out, size_t *consumed);

#ifdef __cplusplus
}
#endif
#endif /* FX_ENTRY_H */
