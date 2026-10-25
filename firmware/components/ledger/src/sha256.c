#include "fx_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

static void compress(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = hh + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void fx_sha256_init(fx_sha256_ctx *ctx)
{
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(ctx->h, H0, sizeof(H0));
    ctx->length_bytes = 0;
    ctx->used = 0;
}

void fx_sha256_update(fx_sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->length_bytes += len;
    while (len > 0) {
        if (ctx->used == 0 && len >= 64) {
            compress(ctx->h, p);
            p += 64;
            len -= 64;
            continue;
        }
        size_t take = 64 - ctx->used;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == 64) {
            compress(ctx->h, ctx->block);
            ctx->used = 0;
        }
    }
}

void fx_sha256_final(fx_sha256_ctx *ctx, uint8_t digest[32])
{
    const uint64_t bits = ctx->length_bytes * 8u;
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, 64 - ctx->used);
        compress(ctx->h, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (int i = 0; i < 8; i++) {
        ctx->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    compress(ctx->h, ctx->block);
    for (int i = 0; i < 8; i++) {
        digest[4 * i] = (uint8_t)(ctx->h[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(ctx->h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(ctx->h[i] >> 8);
        digest[4 * i + 3] = (uint8_t)ctx->h[i];
    }
    memset(ctx, 0, sizeof(*ctx));
}

void fx_sha256(const void *data, size_t len, uint8_t digest[32])
{
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    fx_sha256_update(&ctx, data, len);
    fx_sha256_final(&ctx, digest);
}

void fx_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t mac[32])
{
    uint8_t block[64] = {0};
    if (key_len > sizeof(block)) {
        fx_sha256(key, key_len, block);
    } else if (key_len) {
        memcpy(block, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(block[i] ^ 0x36u);
        opad[i] = (uint8_t)(block[i] ^ 0x5cu);
    }
    uint8_t inner[32];
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    fx_sha256_update(&ctx, ipad, sizeof(ipad));
    fx_sha256_update(&ctx, msg, msg_len);
    fx_sha256_final(&ctx, inner);
    fx_sha256_init(&ctx);
    fx_sha256_update(&ctx, opad, sizeof(opad));
    fx_sha256_update(&ctx, inner, sizeof(inner));
    fx_sha256_final(&ctx, mac);
    memset(block, 0, sizeof(block));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
}
