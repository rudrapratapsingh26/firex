#include "fx_ring.h"

#include <string.h>

typedef enum {
    READ_OK,
    READ_END,     /* clean end of segment */
    READ_BROKEN,  /* bytes that are not a whole valid record */
    READ_ABSENT
} read_result_t;

static read_result_t read_record(fx_ring_t *r, uint32_t seg, uint32_t off, fx_entry_t *e, size_t *consumed)
{
    size_t got = 0;
    if (r->ops.read(r->ops.ctx, seg, off, r->scratch, 4, &got) < 0) {
        return READ_ABSENT;
    }
    if (got == 0) {
        return READ_END;
    }
    if (got < 4) {
        return READ_BROKEN;
    }
    const size_t total = fx_record_size(r->scratch);
    if (total == 0) {
        return READ_BROKEN;
    }
    if (r->ops.read(r->ops.ctx, seg, off, r->scratch, total, &got) < 0 || got < total) {
        return READ_BROKEN;
    }
    return fx_record_decode(r->scratch, total, e, consumed) == FX_REC_OK ? READ_OK : READ_BROKEN;
}

int fx_ring_open(fx_ring_t *r, const fx_store_ops_t *ops, uint32_t seg_bytes, uint32_t max_segs)
{
    if (!r || !ops || !ops->append || !ops->read || !ops->size || !ops->remove || !ops->range) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->ops = *ops;
    r->seg_bytes = seg_bytes < FX_RECORD_MAX ? FX_RECORD_MAX : seg_bytes;
    r->max_segs = max_segs < 2u ? 2u : max_segs;

    uint32_t first = 0, last = 0;
    const int rc = r->ops.range(r->ops.ctx, &first, &last);
    if (rc < 0) {
        return rc;
    }
    if (rc > 0) {
        r->empty = true;
        return 0;
    }
    r->first = first;
    r->last = last;

    fx_entry_t e;
    for (uint32_t seg = last;; seg--) {
        uint32_t off = 0;
        bool found = false;
        size_t consumed = 0;
        read_result_t res;
        while ((res = read_record(r, seg, off, &e, &consumed)) == READ_OK) {
            off += (uint32_t)consumed;
            found = true;
            r->have_last = true;
            r->last_seq = e.seq;
            r->last_ts = e.ts;
            memcpy(r->last_hash, e.hash, 32);
            memcpy(r->last_device_id, e.device_id, 16);
        }
        if (seg == last) {
            const long size = r->ops.size(r->ops.ctx, seg);
            r->last_size = size > 0 ? (uint32_t)size : 0u;
            r->torn = res == READ_BROKEN || (size >= 0 && (uint32_t)size != off);
        }
        if (found || seg == first) {
            break;
        }
    }
    return 0;
}

/* Record the sequence range of a segment about to be removed. */
static void note_removal(fx_ring_t *r, uint32_t seg)
{
    fx_entry_t e;
    uint32_t off = 0;
    size_t consumed = 0;
    while (read_record(r, seg, off, &e, &consumed) == READ_OK) {
        off += (uint32_t)consumed;
        if (!r->removed_pending || e.seq < r->removed_first_seq) {
            r->removed_first_seq = e.seq;
        }
        if (!r->removed_pending || e.seq > r->removed_last_seq) {
            r->removed_last_seq = e.seq;
        }
        r->removed_pending = true;
    }
}

bool fx_ring_take_removed(fx_ring_t *r, uint64_t *first_seq, uint64_t *last_seq)
{
    if (!r->removed_pending) {
        return false;
    }
    *first_seq = r->removed_first_seq;
    *last_seq = r->removed_last_seq;
    r->removed_pending = false;
    return true;
}

int fx_ring_append(fx_ring_t *r, const fx_entry_t *e)
{
    uint8_t buf[FX_RECORD_MAX];
    const size_t n = fx_record_encode(e, buf, sizeof(buf));
    if (n == 0) {
        return -1;
    }

    const bool new_segment = r->empty || r->torn || (r->last_size + n > r->seg_bytes);
    const uint32_t seg = r->empty ? 0u : (new_segment ? r->last + 1u : r->last);
    if (r->ops.append(r->ops.ctx, seg, buf, n) != 0) {
        return -2;
    }

    if (r->empty) {
        r->first = seg;
        r->empty = false;
    }
    if (new_segment) {
        r->last = seg;
        r->last_size = (uint32_t)n;
        r->torn = false;
    } else {
        r->last_size += (uint32_t)n;
    }
    r->have_last = true;
    r->last_seq = e->seq;
    r->last_ts = e->ts;
    memcpy(r->last_hash, e->hash, 32);
    memcpy(r->last_device_id, e->device_id, 16);

    while (r->last - r->first + 1u > r->max_segs) {
        note_removal(r, r->first);
        if (r->ops.remove(r->ops.ctx, r->first) != 0) {
            break;
        }
        r->first++;
        r->removed_segments++;
    }
    return 0;
}

void fx_ring_iter_begin(fx_ring_t *r, fx_ring_iter_t *it)
{
    memset(it, 0, sizeof(*it));
    it->r = r;
    it->seg = r->first;
    it->done = r->empty;
}

int fx_ring_iter_next(fx_ring_iter_t *it, fx_entry_t *e)
{
    while (!it->done) {
        if (it->seg > it->r->last) {
            it->done = true;
            break;
        }
        size_t consumed = 0;
        switch (read_record(it->r, it->seg, it->off, e, &consumed)) {
        case READ_OK:
            it->off += (uint32_t)consumed;
            return 1;
        case READ_BROKEN:
            /* Nothing after an unreadable record can be trusted to align: skip the segment. */
            it->skipped_segments++;
            /* fall through */
        case READ_END:
        case READ_ABSENT:
        default:
            if (it->seg == it->r->last) {
                it->done = true;
            } else {
                it->seg++;
                it->off = 0;
            }
            break;
        }
    }
    return 0;
}
