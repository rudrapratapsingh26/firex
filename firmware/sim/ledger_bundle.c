/*
 * ledger_bundle: push the golden chain through the node's storage and export
 * path, and print the log bundle the reference verifier reads.
 *
 *   entries -> fx_record_encode -> ring (RAM store) -> iterate -> fx_entry_to_json
 *   public key -> fx_spki_pem
 *
 * CI runs `firex-verify verify` on the output and requires PASS, which checks
 * the C record format, ring iteration, JSON export and PEM against the
 * TypeScript verifier end to end. Signing is not exercised here: the golden
 * signatures come from the vector; on the node they come from mbedTLS.
 */
#include <stdio.h>
#include <string.h>

#include "fx_entry.h"
#include "fx_ring.h"
#include "ledger_golden.h"

#define SEGS 16
#define SEG_CAP 8192

static struct {
    bool exists[SEGS];
    size_t len[SEGS];
    uint8_t data[SEGS][SEG_CAP];
} store;

static int s_append(void *ctx, uint32_t seg, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (seg >= SEGS || store.len[seg] + len > SEG_CAP) return -1;
    store.exists[seg] = true;
    memcpy(store.data[seg] + store.len[seg], data, len);
    store.len[seg] += len;
    return 0;
}

static int s_read(void *ctx, uint32_t seg, uint32_t off, uint8_t *buf, size_t len, size_t *got)
{
    (void)ctx;
    if (seg >= SEGS || !store.exists[seg]) return -1;
    const size_t avail = off < store.len[seg] ? store.len[seg] - off : 0;
    *got = len < avail ? len : avail;
    memcpy(buf, store.data[seg] + off, *got);
    return 0;
}

static long s_size(void *ctx, uint32_t seg)
{
    (void)ctx;
    return (seg < SEGS && store.exists[seg]) ? (long)store.len[seg] : -1;
}

static int s_remove(void *ctx, uint32_t seg)
{
    (void)ctx;
    if (seg >= SEGS) return -1;
    store.exists[seg] = false;
    store.len[seg] = 0;
    return 0;
}

static int s_range(void *ctx, uint32_t *first, uint32_t *last)
{
    (void)ctx;
    int lo = -1, hi = -1;
    for (int i = 0; i < SEGS; i++) {
        if (store.exists[i]) {
            if (lo < 0) lo = i;
            hi = i;
        }
    }
    if (lo < 0) return 1;
    *first = (uint32_t)lo;
    *last = (uint32_t)hi;
    return 0;
}

static void print_json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '\n') {
            fputs("\\n", stdout);
        } else if (*s == '"' || *s == '\\') {
            putchar('\\');
            putchar(*s);
        } else {
            putchar(*s);
        }
    }
    putchar('"');
}

int main(void)
{
    static const fx_store_ops_t ops = {s_append, s_read, s_size, s_remove, s_range, NULL};
    static fx_ring_t ring;
    static fx_entry_t e;

    if (fx_ring_open(&ring, &ops, 2048, SEGS) != 0) return 1;
    for (size_t i = 0; i < GOLDEN_COUNT; i++) {
        const golden_entry_t *g = &GOLDEN_ENTRIES[i];
        memset(&e, 0, sizeof(e));
        e.seq = g->seq;
        e.ts = g->ts;
        e.ts_src = (fx_ts_src_t)g->ts_src;
        strcpy(e.type, g->type);
        e.payload_len = g->payload_len;
        memcpy(e.payload, g->payload, g->payload_len);
        memcpy(e.device_id, g->device_id, 16);
        memcpy(e.fw_hash, g->fw_hash, 32);
        memcpy(e.prev_hash, g->prev_hash, 32);
        memcpy(e.hash, g->hash, 32);
        memcpy(e.sig, g->sig, 64);
        if (fx_ring_append(&ring, &e) != 0) return 1;
    }

    char pem[256];
    if (!fx_spki_pem(GOLDEN_PUBKEY, pem, sizeof(pem))) return 1;

    const uint8_t *const media[3] = {GOLDEN_MEDIA_0, GOLDEN_MEDIA_1, GOLDEN_MEDIA_2};
    char hex[65];
    printf("{\"public_key_pem\":");
    print_json_string(pem);
    printf(",\"media\":[");
    for (int i = 0; i < 3; i++) {
        fx_hex_encode(media[i], 32, hex);
        printf("%s\"%s\"", i ? "," : "", hex);
    }
    printf("],\"log\":[");

    static char json[FX_ENTRY_JSON_MAX];
    fx_ring_iter_t it;
    fx_ring_iter_begin(&ring, &it);
    int n = 0;
    while (fx_ring_iter_next(&it, &e) == 1) {
        if (!fx_entry_to_json(&e, json, sizeof(json))) return 1;
        printf("%s%s", n++ ? "," : "", json);
    }
    printf("]}\n");
    return n == (int)GOLDEN_COUNT ? 0 : 1;
}
