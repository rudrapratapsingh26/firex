/*
 * SHA-256 (FIPS 180-4), portable C. Used for ledger entry hashes and media
 * frame hashes on the node, and by the host tests, so both run the same code.
 */
#ifndef FX_SHA256_H
#define FX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t length_bytes;
    uint8_t block[64];
    size_t used;
} fx_sha256_ctx;

void fx_sha256_init(fx_sha256_ctx *ctx);
void fx_sha256_update(fx_sha256_ctx *ctx, const void *data, size_t len);
void fx_sha256_final(fx_sha256_ctx *ctx, uint8_t digest[32]);
void fx_sha256(const void *data, size_t len, uint8_t digest[32]);

/* HMAC-SHA256 (RFC 2104), for the provisioning exchange. */
void fx_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t mac[32]);

#ifdef __cplusplus
}
#endif
#endif /* FX_SHA256_H */
