/*
 * The chain writer: numbers, links, hashes, signs and stores entries, and the
 * continuity check the daily self-test runs over everything on flash.
 */
#ifndef FX_LEDGER_H
#define FX_LEDGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx_entry.h"
#include "fx_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Signs the entry hash, which is SHA-256 of the preimage, so the signature is
 * ECDSA-P256-SHA256 over the preimage exactly as SPEC.md section 2 defines. */
typedef struct {
    int (*sign_digest)(void *ctx, const uint8_t digest[32], uint8_t sig[64]);
    bool (*verify_digest)(void *ctx, const uint8_t digest[32], const uint8_t sig[64]); /* optional */
    void *ctx;
} fx_signer_t;

typedef enum {
    FX_LEDGER_OK = 0,
    FX_LEDGER_BLOCKED,       /* the log on flash belongs to a different device key */
    FX_LEDGER_BAD_EVENT,     /* type or payload too large, or payload not encodable */
    FX_LEDGER_SIGN_FAILED,
    FX_LEDGER_STORE_FAILED
} fx_ledger_status_t;

typedef struct {
    fx_ring_t *ring;
    fx_signer_t signer;
    uint8_t device_id[16];
    uint8_t fw_hash[32];
    uint64_t next_seq;
    uint8_t prev_hash[32];
    uint64_t last_ts;
    bool blocked;
    uint32_t appended;
    uint32_t failures;
} fx_ledger_t;

/* Continues the chain found in the ring. Blocks writing (and returns
 * FX_LEDGER_BLOCKED) if that chain was written under another device key:
 * continuing it would make every later signature fail. */
fx_ledger_status_t fx_ledger_init(fx_ledger_t *l, fx_ring_t *ring, fx_signer_t signer,
                                  const uint8_t device_id[16], const uint8_t fw_hash[32]);

/* `work` is caller-provided scratch (an entry is ~800 bytes) and holds the
 * written entry on return. A timestamp earlier than the previous entry's is
 * raised to it, so ts never runs backwards within a chain. */
fx_ledger_status_t fx_ledger_append(fx_ledger_t *l, const char *type, const uint8_t *payload,
                                    size_t payload_len, uint64_t ts, fx_ts_src_t ts_src, fx_entry_t *work);

typedef struct {
    uint32_t entries;
    bool ok;
    uint64_t failed_seq;
    const char *reason;
    uint32_t skipped_segments;
} fx_chain_report_t;

/* Walk every stored entry: sequence, links, recomputed hashes, and signatures
 * when the signer can verify. */
fx_chain_report_t fx_ledger_check_chain(fx_ring_t *ring, const fx_signer_t *signer, fx_entry_t *work);

const char *fx_ledger_status_name(fx_ledger_status_t s);

#ifdef __cplusplus
}
#endif
#endif /* FX_LEDGER_H */
