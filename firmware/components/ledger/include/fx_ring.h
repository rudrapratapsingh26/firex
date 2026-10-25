/*
 * The on-flash ring buffer: numbered segment files of whole records. New
 * entries append to the newest segment; when the number of segments passes the
 * limit, the oldest segment is removed. Segment numbers only ever increase.
 *
 * The store is an interface so the same logic runs over LittleFS on the node
 * (hw/store_littlefs.c) and over RAM in the host tests. A segment whose tail
 * does not parse (power lost mid-write) is left as it is and never appended to
 * again; the next entry starts a new segment.
 *
 * An exported log that begins after a removed segment still verifies: the
 * reference verifier accepts a first entry with seq > 0 as the starting point.
 */
#ifndef FX_RING_H
#define FX_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Append bytes to a segment, creating it if needed. Durable on return. 0 on success. */
    int (*append)(void *ctx, uint32_t seg, const uint8_t *data, size_t len);
    /* Read up to len bytes at offset. 0 on success with *got possibly short at the end; <0 if absent. */
    int (*read)(void *ctx, uint32_t seg, uint32_t offset, uint8_t *buf, size_t len, size_t *got);
    /* Size in bytes, or -1 if the segment does not exist. */
    long (*size)(void *ctx, uint32_t seg);
    int (*remove)(void *ctx, uint32_t seg);
    /* Lowest and highest existing segment: 0 when some exist, 1 when none, <0 on error. */
    int (*range)(void *ctx, uint32_t *first, uint32_t *last);
    void *ctx;
} fx_store_ops_t;

typedef struct {
    fx_store_ops_t ops;
    uint32_t seg_bytes;
    uint32_t max_segs;

    bool empty;
    uint32_t first;
    uint32_t last;
    uint32_t last_size;
    bool torn; /* the newest segment ends in bytes that are not a record */

    bool have_last;
    uint64_t last_seq;
    uint64_t last_ts;
    uint8_t last_hash[32];
    uint8_t last_device_id[16];

    uint32_t removed_segments; /* since open */
    /* Entries removed by rotation since the last fx_ring_take_removed(). */
    bool removed_pending;
    uint64_t removed_first_seq;
    uint64_t removed_last_seq;
    uint8_t scratch[FX_RECORD_MAX];
} fx_ring_t;

int fx_ring_open(fx_ring_t *r, const fx_store_ops_t *ops, uint32_t seg_bytes, uint32_t max_segs);
int fx_ring_append(fx_ring_t *r, const fx_entry_t *e);

/* The sequence range removed by rotation since the last call. False if nothing
 * was removed. The node logs this as a log_overwrite entry. */
bool fx_ring_take_removed(fx_ring_t *r, uint64_t *first_seq, uint64_t *last_seq);

typedef struct {
    fx_ring_t *r;
    uint32_t seg;
    uint32_t off;
    bool done;
    uint32_t skipped_segments; /* segments abandoned at an unreadable record */
} fx_ring_iter_t;

void fx_ring_iter_begin(fx_ring_t *r, fx_ring_iter_t *it);
/* 1 with an entry, 0 at the end. */
int fx_ring_iter_next(fx_ring_iter_t *it, fx_entry_t *e);

#ifdef __cplusplus
}
#endif
#endif /* FX_RING_H */
