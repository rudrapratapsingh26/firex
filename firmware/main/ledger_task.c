/*
 * The ledger task: drains the event queue, then hashes, signs and stores each
 * entry. Producers never wait: node_log() queues with a zero timeout, and a
 * full queue raises the ledger fault rather than blocking an alarm or fusion
 * task. Each stored entry is echoed as an E line for tools/capture.
 *
 * Serial export (for tools/ledger/export_log.py):
 *   X,begin,<device_id>
 *   X,key,<PEM with newlines written as \n>
 *   X,entry,<EntryJson>          one per entry
 *   X,end,<count>
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "node.h"

static const char *TAG = "ledger";

#define QUEUE_DEPTH 32

typedef struct {
    char type[FX_TYPE_MAX + 1];
    uint16_t len;
    uint32_t t_ms;
    uint8_t payload[FX_PAYLOAD_MAX];
} event_msg_t;

static fx_entry_t s_work;
static char s_json[FX_ENTRY_JSON_MAX];

static void set_fault(bool fault)
{
    portENTER_CRITICAL(&g_node.state_lock);
    g_node.ledger_fault = fault;
    portEXIT_CRITICAL(&g_node.state_lock);
}

bool node_log(const char *type, const uint8_t *payload, size_t len)
{
    if (!g_node.ledger_queue || !type || len == 0 || len > FX_PAYLOAD_MAX || strlen(type) > FX_TYPE_MAX) {
        return false;
    }
    event_msg_t msg;
    snprintf(msg.type, sizeof(msg.type), "%s", type);
    msg.len = (uint16_t)len;
    msg.t_ms = node_now_ms();
    memcpy(msg.payload, payload, len);
    if (xQueueSend(g_node.ledger_queue, &msg, 0) != pdTRUE) {
        portENTER_CRITICAL(&g_node.state_lock);
        g_node.ledger_dropped++;
        g_node.ledger_fault = true;
        portEXIT_CRITICAL(&g_node.state_lock);
        return false;
    }
    return true;
}

void node_echo_event(const char *json_body)
{
    printf("E,%" PRIu32 ",{%s}\n", node_now_ms(), json_body);
    fflush(stdout);
}

/* E,<t_ms>,{"type":"...","seq":N,<payload fields>} - the M1 event shape, so the
 * capture tool and notebooks read ledger events the same way as before. */
static void echo_entry(const fx_entry_t *e, uint32_t t_ms)
{
    char payload_json[1200];
    if (!fx_cbor_to_json(e->payload, e->payload_len, payload_json, sizeof(payload_json)) || payload_json[0] != '{') {
        return;
    }
    const char *fields = payload_json + 1; /* after the opening brace */
    printf("E,%" PRIu32 ",{\"type\":\"%s\",\"seq\":%" PRIu64 "%s%s\n", t_ms, e->type, e->seq,
           fields[0] == '}' ? "" : ",", fields);
    fflush(stdout);
}

static void ledger_task(void *arg)
{
    (void)arg;
    event_msg_t msg;
    for (;;) {
        if (xQueueReceive(g_node.ledger_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!g_node.ledger_ok) {
            set_fault(true);
            continue;
        }
        xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
        fx_ts_src_t src = FX_TS_UNCERTAIN;
        const uint64_t ts = fx_clock_stamp(&g_node.clock, msg.t_ms, &src);
        const fx_ledger_status_t st =
            fx_ledger_append(&g_node.ledger, msg.type, msg.payload, msg.len, ts, src, &s_work);
        uint64_t removed_first = 0, removed_last = 0;
        const bool removed = fx_ring_take_removed(&g_node.ring_buffer, &removed_first, &removed_last);
        xSemaphoreGive(g_node.ledger_mutex);

        if (removed) {
            /* Rotation dropped entries to make room: the signed record says so, and
             * whether the gateway already held them (ledger SPEC v0.4). */
            static uint8_t overwrite[FX_PAYLOAD_MAX];
            const bool mirrored = node_net_acked() >= (int64_t)removed_last;
            if (!mirrored) {
                ESP_LOGW(TAG, "entries %" PRIu64 "-%" PRIu64 " were overwritten before the gateway had them",
                         removed_first, removed_last);
            }
            node_log("log_overwrite", overwrite, fx_ev_log_overwrite(overwrite, removed_first, removed_last, mirrored));
        }

        if (st == FX_LEDGER_OK) {
            echo_entry(&s_work, msg.t_ms);
        } else {
            set_fault(true);
            ESP_LOGE(TAG, "%s not written: %s", msg.type, fx_ledger_status_name(st));
            char body[96];
            snprintf(body, sizeof(body), "\"type\":\"ledger_error\",\"event\":\"%s\",\"reason\":\"%s\"", msg.type,
                     fx_ledger_status_name(st));
            node_echo_event(body);
        }
    }
}

void node_ledger_start(void)
{
    g_node.ledger_queue = xQueueCreate(QUEUE_DEPTH, sizeof(event_msg_t));
    g_node.ledger_mutex = xSemaphoreCreateMutex();
    xTaskCreate(ledger_task, "ledger", 8192, NULL, 5, NULL);
}

void node_ledger_print_status(void)
{
    char id[33];
    fx_hex_encode(g_node.key.device_id, 16, id);
    size_t total = 0, used = 0;
    fx_fs_usage("ledger", &total, &used);
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    const fx_ring_t *r = &g_node.ring_buffer;
    printf("device_id %s\n", g_node.key_ok ? id : "(no key)");
    printf("ledger %s%s, next seq %" PRIu64 ", appended this boot %" PRIu32 ", failures %" PRIu32 "\n",
           g_node.ledger_ok ? "open" : "NOT OPEN", g_node.ledger.blocked ? " (BLOCKED: written by another key)" : "",
           g_node.ledger.next_seq, g_node.ledger.appended, g_node.ledger.failures);
    printf("segments %" PRIu32 "-%" PRIu32 "%s, removed this boot %" PRIu32 ", partition %u of %u KiB used\n",
           r->first, r->last, r->empty ? " (empty)" : "", r->removed_segments, (unsigned)(used / 1024),
           (unsigned)(total / 1024));
    fx_ts_src_t src = FX_TS_UNCERTAIN;
    const uint64_t ts = fx_clock_now(&g_node.clock, node_now_ms(), &src);
    printf("queue dropped %" PRIu32 ", time source %s, ts %" PRIu64 "\n", g_node.ledger_dropped, fx_ts_src_name(src),
           ts);
    xSemaphoreGive(g_node.ledger_mutex);
}

static uint32_t s_clock_syncs;
static uint32_t s_clock_synced_ms;
static uint64_t s_clock_step_ms;
static bool s_clock_forward;

void node_clock_sync(uint64_t unix_ms)
{
    const uint32_t now = node_now_ms();
    uint64_t step = 0;
    bool forward = true;
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    const bool log = fx_clock_sync(&g_node.clock, now, unix_ms, &step, &forward);
    xSemaphoreGive(g_node.ledger_mutex);
    s_clock_syncs++;
    s_clock_synced_ms = now;
    s_clock_step_ms = step;
    s_clock_forward = forward;
    if (log) {
        /* Stamped with the new time: from here on this boot's entries say `gateway`. */
        uint8_t payload[FX_PAYLOAD_MAX];
        node_log("clock_sync", payload, fx_ev_clock_sync(payload, step, forward));
        ESP_LOGI(TAG, "clock set from the gateway: %s %" PRIu64 " ms", forward ? "forward" : "back", step);
    }
}

void node_clock_print(void)
{
    fx_ts_src_t src = FX_TS_UNCERTAIN;
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    const uint64_t ts = fx_clock_now(&g_node.clock, node_now_ms(), &src);
    const uint64_t floor_s = g_node.clock.floor_s;
    xSemaphoreGive(g_node.ledger_mutex);
    printf("clock    ts %" PRIu64 " (%s); the newest entry has ts %" PRIu64 "\n", ts, fx_ts_src_name(src), floor_s);
    if (s_clock_syncs == 0) {
        printf("         not set by the gateway on this boot: entries are flagged uncertain\n");
        return;
    }
    printf("         %" PRIu32 " gateway syncs this boot; the last, %" PRIu32 " s ago, moved it %s by %" PRIu64 " ms\n",
           s_clock_syncs, (node_now_ms() - s_clock_synced_ms) / 1000u, s_clock_forward ? "forward" : "back",
           s_clock_step_ms);
}

/* Walk entries one at a time, holding the mutex only per entry, so a long
 * export over a slow UART never stalls the writer. */
typedef void (*visit_fn)(const fx_entry_t *e, void *ctx);

static uint32_t walk(uint64_t from_seq, visit_fn fn, void *ctx)
{
    static fx_entry_t e;
    fx_ring_iter_t it;
    uint32_t n = 0;
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    fx_ring_iter_begin(&g_node.ring_buffer, &it);
    xSemaphoreGive(g_node.ledger_mutex);
    for (;;) {
        xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
        const int got = fx_ring_iter_next(&it, &e);
        xSemaphoreGive(g_node.ledger_mutex);
        if (got != 1) {
            break;
        }
        if (e.seq >= from_seq) {
            fn(&e, ctx);
            n++;
        }
    }
    return n;
}

static void print_entry(const fx_entry_t *e, void *ctx)
{
    const char *prefix = (const char *)ctx;
    if (fx_entry_to_json(e, s_json, sizeof(s_json))) {
        printf("%s%s\n", prefix, s_json);
        fflush(stdout);
    }
}

void node_ledger_tail(uint32_t count)
{
    const uint64_t next = g_node.ledger.next_seq;
    const uint64_t from = next > count ? next - count : 0u;
    walk(from, print_entry, (void *)"");
}

void node_ledger_export(uint64_t from_seq)
{
    char id[33], pem[256];
    fx_hex_encode(g_node.key.device_id, 16, id);
    printf("X,begin,%s\n", id);
    if (fx_device_key_pem(&g_node.key, pem, sizeof(pem))) {
        printf("X,key,");
        for (const char *p = pem; *p; p++) {
            fputs(*p == '\n' ? "\\n" : (char[2]){*p, 0}, stdout);
        }
        printf("\n");
    }
    const uint32_t n = walk(from_seq, print_entry, (void *)"X,entry,");
    printf("X,end,%" PRIu32 "\n", n);
    fflush(stdout);
}

bool node_ledger_check(bool signatures, fx_chain_report_t *report)
{
    fx_signer_t signer = fx_device_key_signer(&g_node.key);
    if (!signatures) {
        signer.verify_digest = NULL;
    }
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    *report = fx_ledger_check_chain(&g_node.ring_buffer, &signer, &s_work);
    xSemaphoreGive(g_node.ledger_mutex);
    return report->ok;
}

typedef struct {
    fx_entry_t newest;
    bool have;
} newest_t;

static void keep_newest(const fx_entry_t *e, void *ctx)
{
    newest_t *n = (newest_t *)ctx;
    n->newest = *e;
    n->have = true;
}

bool node_ledger_verify_newest(void)
{
    static newest_t n;
    n.have = false;
    const uint64_t next = g_node.ledger.next_seq;
    walk(next > 0 ? next - 1u : 0u, keep_newest, &n);
    if (!n.have) {
        return true; /* nothing written yet: nothing to fail */
    }
    fx_signer_t signer = fx_device_key_signer(&g_node.key);
    return signer.verify_digest(signer.ctx, n.newest.hash, n.newest.sig);
}

int node_ledger_wipe(void)
{
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    const int removed = fx_fs_store_wipe(&g_node.store);
    fx_store_ops_t ops;
    fx_fs_store_init(&g_node.store, NODE_LEDGER_BASE, &ops);
    fx_ring_open(&g_node.ring_buffer, &ops, NODE_LEDGER_SEG_BYTES, NODE_LEDGER_MAX_SEGS);
    const fx_ledger_status_t st = fx_ledger_init(&g_node.ledger, &g_node.ring_buffer,
                                                 fx_device_key_signer(&g_node.key), g_node.key.device_id,
                                                 g_node.fw_hash);
    g_node.ledger_ok = g_node.key_ok && st == FX_LEDGER_OK;
    xSemaphoreGive(g_node.ledger_mutex);
    set_fault(!g_node.ledger_ok);
    return removed;
}
