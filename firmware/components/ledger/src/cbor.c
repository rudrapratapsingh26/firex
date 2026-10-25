#include "fx_cbor.h"

#include <string.h>

void fx_cbor_init(fx_cbor_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

static void put(fx_cbor_t *w, const uint8_t *data, size_t len)
{
    if (w->overflow || len > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
}

static size_t head_bytes(uint8_t major, uint64_t v, uint8_t out[9])
{
    const uint8_t m = (uint8_t)(major << 5);
    if (v < 24u) {
        out[0] = (uint8_t)(m | v);
        return 1;
    }
    if (v <= 0xFFu) {
        out[0] = (uint8_t)(m | 24u);
        out[1] = (uint8_t)v;
        return 2;
    }
    if (v <= 0xFFFFu) {
        out[0] = (uint8_t)(m | 25u);
        out[1] = (uint8_t)(v >> 8);
        out[2] = (uint8_t)v;
        return 3;
    }
    if (v <= 0xFFFFFFFFu) {
        out[0] = (uint8_t)(m | 26u);
        for (int i = 0; i < 4; i++) {
            out[1 + i] = (uint8_t)(v >> (24 - 8 * i));
        }
        return 5;
    }
    out[0] = (uint8_t)(m | 27u);
    for (int i = 0; i < 8; i++) {
        out[1 + i] = (uint8_t)(v >> (56 - 8 * i));
    }
    return 9;
}

void fx_cbor_head(fx_cbor_t *w, uint8_t major, uint64_t value)
{
    uint8_t h[9];
    put(w, h, head_bytes(major, value, h));
}

void fx_cbor_uint(fx_cbor_t *w, uint64_t value)
{
    fx_cbor_head(w, 0, value);
}

void fx_cbor_text_n(fx_cbor_t *w, const char *text, size_t len)
{
    fx_cbor_head(w, 3, len);
    put(w, (const uint8_t *)text, len);
}

void fx_cbor_text(fx_cbor_t *w, const char *text)
{
    fx_cbor_text_n(w, text, strlen(text));
}

void fx_cbor_bytes(fx_cbor_t *w, const uint8_t *data, size_t len)
{
    fx_cbor_head(w, 2, len);
    put(w, data, len);
}

void fx_cbor_bool(fx_cbor_t *w, bool value)
{
    const uint8_t b = value ? 0xF5 : 0xF4;
    put(w, &b, 1);
}

void fx_cbor_null(fx_cbor_t *w)
{
    const uint8_t b = 0xF6;
    put(w, &b, 1);
}

void fx_cbor_array(fx_cbor_t *w, size_t count)
{
    fx_cbor_head(w, 4, count);
}

void fx_cbor_raw(fx_cbor_t *w, const uint8_t *cbor, size_t len)
{
    put(w, cbor, len);
}

/* --- maps ------------------------------------------------------------------ */

void fx_map_init(fx_map_t *m)
{
    m->count = 0;
    m->error = false;
    fx_cbor_init(&m->w, m->scratch, sizeof(m->scratch));
}

static bool begin(fx_map_t *m, const char *key, size_t *off)
{
    if (m->error || !key || m->count >= FX_MAP_MAX_FIELDS) {
        m->error = true;
        return false;
    }
    *off = m->w.len;
    return true;
}

static void end(fx_map_t *m, const char *key, size_t off)
{
    if (m->w.overflow) {
        m->error = true;
        return;
    }
    m->field[m->count].key = key;
    m->field[m->count].off = (uint16_t)off;
    m->field[m->count].len = (uint16_t)(m->w.len - off);
    m->count++;
}

void fx_map_uint(fx_map_t *m, const char *key, uint64_t value)
{
    size_t off;
    if (begin(m, key, &off)) {
        fx_cbor_uint(&m->w, value);
        end(m, key, off);
    }
}

void fx_map_text(fx_map_t *m, const char *key, const char *value)
{
    size_t off;
    if (begin(m, key, &off)) {
        fx_cbor_text(&m->w, value ? value : "");
        end(m, key, off);
    }
}

void fx_map_bytes(fx_map_t *m, const char *key, const uint8_t *data, size_t len)
{
    size_t off;
    if (begin(m, key, &off)) {
        fx_cbor_bytes(&m->w, data, len);
        end(m, key, off);
    }
}

void fx_map_bool(fx_map_t *m, const char *key, bool value)
{
    size_t off;
    if (begin(m, key, &off)) {
        fx_cbor_bool(&m->w, value);
        end(m, key, off);
    }
}

void fx_map_null(fx_map_t *m, const char *key)
{
    size_t off;
    if (begin(m, key, &off)) {
        fx_cbor_null(&m->w);
        end(m, key, off);
    }
}

void fx_map_raw(fx_map_t *m, const char *key, const uint8_t *cbor, size_t len)
{
    size_t off;
    if (begin(m, key, &off)) {
        fx_cbor_raw(&m->w, cbor, len);
        end(m, key, off);
    }
}

/* Compare two text keys by their canonical CBOR encoding (head, then bytes). */
static int key_compare(const char *a, const char *b)
{
    uint8_t ha[9], hb[9];
    const size_t la = strlen(a), lb = strlen(b);
    const size_t na = head_bytes(3, la, ha), nb = head_bytes(3, lb, hb);
    const size_t ta = na + la, tb = nb + lb;
    const size_t n = ta < tb ? ta : tb;
    for (size_t i = 0; i < n; i++) {
        const uint8_t ca = i < na ? ha[i] : (uint8_t)a[i - na];
        const uint8_t cb = i < nb ? hb[i] : (uint8_t)b[i - nb];
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
    }
    return (ta > tb) - (ta < tb);
}

size_t fx_map_finish(fx_map_t *m, uint8_t *out, size_t cap)
{
    if (m->error) {
        return 0;
    }
    uint8_t order[FX_MAP_MAX_FIELDS];
    for (uint8_t i = 0; i < m->count; i++) {
        order[i] = i;
    }
    for (uint8_t i = 1; i < m->count; i++) {
        const uint8_t cur = order[i];
        int j = (int)i - 1;
        while (j >= 0 && key_compare(m->field[order[j]].key, m->field[cur].key) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }
    for (uint8_t i = 1; i < m->count; i++) {
        if (key_compare(m->field[order[i - 1]].key, m->field[order[i]].key) == 0) {
            return 0; /* duplicate key: not a valid canonical map */
        }
    }

    fx_cbor_t w;
    fx_cbor_init(&w, out, cap);
    fx_cbor_head(&w, 5, m->count);
    for (uint8_t i = 0; i < m->count; i++) {
        const uint8_t k = order[i];
        fx_cbor_text(&w, m->field[k].key);
        fx_cbor_raw(&w, m->scratch + m->field[k].off, m->field[k].len);
    }
    return w.overflow ? 0 : w.len;
}
