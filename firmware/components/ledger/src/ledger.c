#include "fx_ledger.h"

#include <string.h>

static const uint8_t ZERO32[32] = {0};

fx_ledger_status_t fx_ledger_init(fx_ledger_t *l, fx_ring_t *ring, fx_signer_t signer,
                                  const uint8_t device_id[16], const uint8_t fw_hash[32])
{
    memset(l, 0, sizeof(*l));
    l->ring = ring;
    l->signer = signer;
    memcpy(l->device_id, device_id, 16);
    memcpy(l->fw_hash, fw_hash, 32);
    if (ring->have_last) {
        l->next_seq = ring->last_seq + 1u;
        memcpy(l->prev_hash, ring->last_hash, 32);
        l->last_ts = ring->last_ts;
        if (memcmp(ring->last_device_id, device_id, 16) != 0) {
            l->blocked = true;
            return FX_LEDGER_BLOCKED;
        }
    }
    return FX_LEDGER_OK;
}

fx_ledger_status_t fx_ledger_append(fx_ledger_t *l, const char *type, const uint8_t *payload,
                                    size_t payload_len, uint64_t ts, fx_ts_src_t ts_src, fx_entry_t *work)
{
    if (l->blocked) {
        l->failures++;
        return FX_LEDGER_BLOCKED;
    }
    if (!type || !work || strlen(type) > FX_TYPE_MAX || payload_len > FX_PAYLOAD_MAX || (!payload && payload_len)) {
        l->failures++;
        return FX_LEDGER_BAD_EVENT;
    }

    fx_entry_t *e = work;
    memset(e, 0, sizeof(*e));
    e->seq = l->next_seq;
    e->ts = ts < l->last_ts ? l->last_ts : ts;
    e->ts_src = ts_src;
    memcpy(e->device_id, l->device_id, 16);
    strcpy(e->type, type);
    if (payload_len) {
        memcpy(e->payload, payload, payload_len);
    }
    e->payload_len = (uint16_t)payload_len;
    memcpy(e->fw_hash, l->fw_hash, 32);
    memcpy(e->prev_hash, l->prev_hash, 32);

    if (!fx_entry_compute_hash(e, e->hash)) {
        l->failures++;
        return FX_LEDGER_BAD_EVENT;
    }
    if (!l->signer.sign_digest || l->signer.sign_digest(l->signer.ctx, e->hash, e->sig) != 0) {
        l->failures++;
        return FX_LEDGER_SIGN_FAILED;
    }
    if (fx_ring_append(l->ring, e) != 0) {
        l->failures++;
        return FX_LEDGER_STORE_FAILED;
    }

    l->next_seq++;
    memcpy(l->prev_hash, e->hash, 32);
    l->last_ts = e->ts;
    l->appended++;
    return FX_LEDGER_OK;
}

fx_chain_report_t fx_ledger_check_chain(fx_ring_t *ring, const fx_signer_t *signer, fx_entry_t *work)
{
    fx_chain_report_t rep = {0, true, 0, NULL, 0};
    fx_ring_iter_t it;
    fx_ring_iter_begin(ring, &it);

    bool have_prev = false;
    uint64_t prev_seq = 0;
    uint8_t prev_hash[32];
    uint8_t recomputed[32];

    while (fx_ring_iter_next(&it, work) == 1) {
        const fx_entry_t *e = work;
        const char *reason = NULL;
        if (!have_prev) {
            if (e->seq == 0u && memcmp(e->prev_hash, ZERO32, 32) != 0) {
                reason = "genesis entry has a non-zero prev_hash";
            } else if (e->seq != 0u && memcmp(e->prev_hash, ZERO32, 32) == 0) {
                reason = "entry with a zero prev_hash is not seq 0";
            }
        } else if (e->seq != prev_seq + 1u) {
            reason = "sequence gap";
        } else if (memcmp(e->prev_hash, prev_hash, 32) != 0) {
            reason = "prev_hash does not match the previous entry";
        }
        if (!reason && (!fx_entry_compute_hash(e, recomputed) || memcmp(recomputed, e->hash, 32) != 0)) {
            reason = "hash does not match the entry's contents";
        }
        if (!reason && signer && signer->verify_digest && !signer->verify_digest(signer->ctx, e->hash, e->sig)) {
            reason = "signature does not verify";
        }
        rep.entries++;
        if (reason) {
            rep.ok = false;
            rep.failed_seq = e->seq;
            rep.reason = reason;
            break;
        }
        have_prev = true;
        prev_seq = e->seq;
        memcpy(prev_hash, e->hash, 32);
    }
    rep.skipped_segments = it.skipped_segments;
    if (rep.ok && it.skipped_segments) {
        rep.ok = false;
        rep.reason = "a segment holds bytes that are not a valid record";
    }
    return rep;
}

const char *fx_ledger_status_name(fx_ledger_status_t s)
{
    switch (s) {
    case FX_LEDGER_OK: return "ok";
    case FX_LEDGER_BLOCKED: return "blocked";
    case FX_LEDGER_BAD_EVENT: return "bad_event";
    case FX_LEDGER_SIGN_FAILED: return "sign_failed";
    case FX_LEDGER_STORE_FAILED: return "store_failed";
    default: return "?";
    }
}
