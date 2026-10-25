/*
 * Canonical CBOR writer for the ledger subset (SPEC.md section 4): unsigned
 * integers, text and byte strings, arrays, maps, booleans and null, all with
 * definite lengths and shortest-form heads. Floats and negative integers are
 * not representable on purpose.
 *
 * fx_map_t collects fields in any order and writes them sorted by the encoded
 * bytes of their keys, which is the ordering rule that makes the bytes (and so
 * the hash) identical to ledger/src/cbor.ts.
 */
#ifndef FX_CBOR_H
#define FX_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow; /* sticky: any write that did not fit */
} fx_cbor_t;

void fx_cbor_init(fx_cbor_t *w, uint8_t *buf, size_t cap);
void fx_cbor_head(fx_cbor_t *w, uint8_t major, uint64_t value);
void fx_cbor_uint(fx_cbor_t *w, uint64_t value);
void fx_cbor_text(fx_cbor_t *w, const char *text);
void fx_cbor_text_n(fx_cbor_t *w, const char *text, size_t len);
void fx_cbor_bytes(fx_cbor_t *w, const uint8_t *data, size_t len);
void fx_cbor_bool(fx_cbor_t *w, bool value);
void fx_cbor_null(fx_cbor_t *w);
void fx_cbor_array(fx_cbor_t *w, size_t count); /* head only; write `count` items next */
void fx_cbor_raw(fx_cbor_t *w, const uint8_t *cbor, size_t len);

/* --- maps with canonical key order --------------------------------------- */

#define FX_MAP_MAX_FIELDS 16u
#define FX_MAP_SCRATCH 512u

typedef struct {
    uint8_t scratch[FX_MAP_SCRATCH];
    fx_cbor_t w;
    struct {
        const char *key; /* must outlive the map: string literals in practice */
        uint16_t off;
        uint16_t len;
    } field[FX_MAP_MAX_FIELDS];
    uint8_t count;
    bool error;
} fx_map_t;

void fx_map_init(fx_map_t *m);
void fx_map_uint(fx_map_t *m, const char *key, uint64_t value);
void fx_map_text(fx_map_t *m, const char *key, const char *value);
void fx_map_bytes(fx_map_t *m, const char *key, const uint8_t *data, size_t len);
void fx_map_bool(fx_map_t *m, const char *key, bool value);
void fx_map_null(fx_map_t *m, const char *key);
/* An already-encoded value: a nested map, an array. */
void fx_map_raw(fx_map_t *m, const char *key, const uint8_t *cbor, size_t len);

/* Write the map with its keys in canonical order. Returns the encoded length,
 * or 0 when a field overflowed, a key repeated, or `out` is too small. */
size_t fx_map_finish(fx_map_t *m, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* FX_CBOR_H */
