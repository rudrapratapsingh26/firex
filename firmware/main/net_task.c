/*
 * The network task (M3). It joins the site Wi-Fi, provisions the unit with the
 * gateway once, then keeps an MQTT connection over mutual TLS and mirrors the
 * node to the gateway (docs/transport.md):
 *
 *   hello        on every connection: what the ledger on flash holds
 *   state        on every change and every CONFIG_FIREX_NET_STATE_S, retained
 *   telemetry    every CONFIG_FIREX_NET_TELEMETRY_S, QoS 0
 *   ledger       entries from the gateway's sync point, at most SEND_WINDOW past its last ack
 *   media/<sha>  each photo on the media partition, deleted once the gateway acknowledges it
 *   trace/<sha>  each episode trace, after the photos, deleted once acknowledged (M5)
 *
 * From M5 it also takes the gateway's time for the ledger clock, and firmware
 * offers, which it hands to the OTA task.
 *
 * The ledger on flash is the outbound queue. Nothing is deleted because it was
 * sent, so a Wi-Fi outage, a reboot or a gateway restart loses nothing the ring
 * still holds. This task runs below the alarm, fusion, sensor and ledger tasks,
 * and nothing they do waits on the network.
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "fx_net_hw.h"
#include "fx_sha256.h"
#include "fx_sync.h"
#include "mqtt_client.h"
#include "node.h"
#include "sdkconfig.h"

static const char *TAG = "net";

void node_net_identity(void)
{
    g_node.tls_ok = fx_tls_key_open(&g_node.tls_key) == ESP_OK;
    if (g_node.tls_key.created) {
        ESP_LOGW(TAG, "a new TLS key was generated on this boot");
    }
    g_node.pairing_ok = fx_net_pairing_code(g_node.pairing_code) == ESP_OK;
    if (g_node.pairing_ok) {
        char formatted[12];
        fx_pairing_code_format(g_node.pairing_code, formatted);
        printf("pairing code %s\n", formatted);
    } else {
        ESP_LOGE(TAG, "no pairing code: this unit cannot be added to a site");
    }
}

#if CONFIG_FIREX_NET_ENABLE

#define SEND_WINDOW 8u
#define LOOP_MS 200u
#define PROVISION_RETRY_MS 30000u
#define MQTT_RETRY_MS 5000u
#define MEDIA_ACK_WAIT_MS 60000u
#define MEDIA_MAX_BYTES (2u * 1024u * 1024u)
#define STATE_MS (CONFIG_FIREX_NET_STATE_S * 1000u)
#define TELEMETRY_MS (CONFIG_FIREX_NET_TELEMETRY_S * 1000u)
#define CMD_QUEUE_DEPTH 16

typedef struct {
    bool have_fusion;
    fx_state_t fusion;
    fx_alarm_state_t alarm;
    bool sensor;
    bool ledger;
    bool selftest;
    bool buzzer;
} node_state_t;

typedef struct {
    fx_ring_iter_t it;
    bool valid;
    uint32_t first_seg;
    int64_t last_seq; /* the seq the iterator yielded last, -1 before any */
} cursor_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_cmds;
static esp_mqtt_client_handle_t s_client;
static fx_net_creds_t s_creds;
static volatile bool s_have_creds;
static char s_topic_prefix[96];
static char s_client_id[33];
static volatile bool s_mqtt_up;
static volatile uint32_t s_connections;
static volatile bool s_provision_requested;
static volatile bool s_forget_requested;
static volatile bool s_forget_gateway;
static volatile bool s_provision_tried;
static volatile fx_prov_outcome_t s_provision_outcome;

static fx_sync_t s_sync;
static int64_t s_acked = -1; /* under s_lock */
static uint32_t s_entries_sent;
static uint32_t s_media_sent;
static uint32_t s_media_deleted;

static cursor_t s_cursor;
static int64_t s_oldest = -1;
static uint32_t s_oldest_first = UINT32_MAX;
static fx_entry_t s_entry;
static char s_json[FX_ENTRY_JSON_MAX];

static bool s_media_waiting;
static uint32_t s_media_sent_ms;
static char s_media_pending[65];
static char s_media_last[64];
static bool s_photos_present; /* the last scan found a photo to send */

#define TRACE_MAX_BYTES (1024u * 1024u)

static bool s_trace_waiting;
static uint32_t s_trace_sent_ms;
static char s_trace_pending[65];
static char s_trace_last[64];
static uint32_t s_traces_sent;
static uint32_t s_traces_deleted;

/* --- small helpers ------------------------------------------------------------- */

static bool appendf(char *buf, size_t cap, size_t *n, const char *fmt, ...)
{
    if (*n >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - *n) {
        *n = cap;
        return false;
    }
    *n += (size_t)w;
    return true;
}

static const char *json_bool(bool b)
{
    return b ? "true" : "false";
}

static int publish(const char *leaf, const char *data, size_t len, int qos, bool retain)
{
    char topic[168];
    snprintf(topic, sizeof(topic), "%s%s", s_topic_prefix, leaf);
    return esp_mqtt_client_publish(s_client, topic, data, (int)len, qos, retain ? 1 : 0);
}

int64_t node_net_acked(void)
{
    portENTER_CRITICAL(&s_lock);
    const int64_t v = s_acked;
    portEXIT_CRITICAL(&s_lock);
    return v;
}

bool node_net_synced(void)
{
    return s_mqtt_up && s_sync.synced;
}

const fx_net_creds_t *node_net_creds(void)
{
    return s_have_creds ? &s_creds : NULL;
}

static void set_acked(int64_t v)
{
    portENTER_CRITICAL(&s_lock);
    s_acked = v;
    portEXIT_CRITICAL(&s_lock);
}

/* --- the ledger as the outbound queue ------------------------------------------- */

typedef struct {
    bool ok;        /* the ledger is open */
    uint64_t next;  /* next seq the ledger will write */
    int64_t oldest; /* oldest seq on flash, -1 for none */
    int64_t last;   /* newest seq on flash, -1 for none */
} bounds_t;

static bounds_t ledger_bounds(void)
{
    bounds_t b = {false, 0, -1, -1};
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    b.ok = g_node.ledger_ok;
    b.next = g_node.ledger.next_seq;
    const bool have = b.ok && !g_node.ring_buffer.empty && g_node.ring_buffer.have_last;
    const uint32_t first = g_node.ring_buffer.first;
    /* The oldest entry changes only when a segment is removed, or the log is wiped. */
    if (have && (first != s_oldest_first || s_oldest < 0 || (int64_t)b.next <= s_oldest)) {
        fx_ring_iter_t it;
        fx_ring_iter_begin(&g_node.ring_buffer, &it);
        s_oldest = fx_ring_iter_next(&it, &s_entry) == 1 ? (int64_t)s_entry.seq : -1;
        s_oldest_first = first;
    }
    xSemaphoreGive(g_node.ledger_mutex);
    if (have) {
        b.oldest = s_oldest;
        b.last = b.next > 0 ? (int64_t)b.next - 1 : -1;
    }
    return b;
}

/* The first entry with seq >= `seq`. The cursor stays where it stopped, so
 * sending entries in order never rescans the ring. */
static bool read_from(uint64_t seq, fx_entry_t *e)
{
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    const uint32_t first = g_node.ring_buffer.first;
    if (!s_cursor.valid || s_cursor.first_seg != first || s_cursor.last_seq >= (int64_t)seq) {
        fx_ring_iter_begin(&g_node.ring_buffer, &s_cursor.it);
        s_cursor.valid = true;
        s_cursor.first_seg = first;
        s_cursor.last_seq = -1;
    }
    xSemaphoreGive(g_node.ledger_mutex);

    for (;;) {
        xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
        const fx_ring_iter_t before = s_cursor.it;
        const int got = fx_ring_iter_next(&s_cursor.it, e);
        if (got != 1) {
            /* At the end: stay here, so entries written later are read from this point. */
            s_cursor.it = before;
            s_cursor.it.done = false;
        }
        xSemaphoreGive(g_node.ledger_mutex);
        if (got != 1) {
            return false;
        }
        s_cursor.last_seq = (int64_t)e->seq;
        if (e->seq >= seq) {
            return true;
        }
    }
}

static void pump_ledger(void)
{
    const bounds_t b = ledger_bounds();
    if (!b.ok) {
        return;
    }
    for (uint32_t i = 0; i < SEND_WINDOW && s_mqtt_up; i++) {
        const int64_t n = fx_sync_next(&s_sync, b.oldest, b.last);
        if (n < 0 || !read_from((uint64_t)n, &s_entry)) {
            return;
        }
        const size_t len = fx_entry_to_json(&s_entry, s_json, sizeof(s_json));
        if (len == 0) {
            ESP_LOGE(TAG, "entry %" PRIu64 " could not be written as JSON; skipped", s_entry.seq);
        } else if (publish("ledger", s_json, len, 1, false) < 0) {
            return; /* the connection dropped: the next sync decides where to resume */
        } else {
            s_entries_sent++;
        }
        fx_sync_sent(&s_sync, (int64_t)s_entry.seq);
    }
}

/* --- photos --------------------------------------------------------------------- */

static bool hash_file(const char *path, uint8_t digest[32])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    uint8_t buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fx_sha256_update(&ctx, buf, n);
    }
    const bool ok = !ferror(f);
    fclose(f);
    fx_sha256_final(&ctx, digest);
    return ok;
}

static bool is_photo(const char *name)
{
    const size_t len = strlen(name);
    return len > 4u && len < sizeof(s_media_last) && strcmp(name + len - 4u, ".jpg") == 0;
}

/* The gateway holds the photo and checked its hash: the node's copy can go. */
static void on_media_ack(const char *sha)
{
    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (dir) {
        struct dirent *d;
        char path[128];
        while ((d = readdir(dir)) != NULL) {
            /* Names end in the first 16 hex digits of the hash; the whole file is hashed to be sure. */
            if (!is_photo(d->d_name)) {
                continue;
            }
            char prefix[17];
            memcpy(prefix, sha, 16);
            prefix[16] = '\0';
            if (!strstr(d->d_name, prefix)) {
                continue;
            }
            snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%.63s", d->d_name);
            uint8_t digest[32];
            char hex[65];
            if (hash_file(path, digest)) {
                fx_hex_encode(digest, 32, hex);
                if (strcmp(hex, sha) == 0 && unlink(path) == 0) {
                    s_media_deleted++;
                    ESP_LOGI(TAG, "photo %.16s is on the gateway; deleted here", sha);
                    break;
                }
            }
        }
        closedir(dir);
    }
    if (s_media_waiting && strcmp(sha, s_media_pending) == 0) {
        s_media_waiting = false;
    }
}

static void pump_media(uint32_t now)
{
    if (!g_node.media_ok || !s_mqtt_up || !s_sync.synced) {
        return;
    }
    if (s_media_waiting && now - s_media_sent_ms < MEDIA_ACK_WAIT_MS) {
        return;
    }
    s_media_waiting = false;

    /* Take photos in name order, carrying on after the last one tried, so one the
     * gateway will not take never holds up the rest. */
    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (!dir) {
        return;
    }
    char after[64] = "", lowest[64] = "";
    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        if (!is_photo(d->d_name)) {
            continue;
        }
        if (strcmp(d->d_name, s_media_last) > 0 && (!after[0] || strcmp(d->d_name, after) < 0)) {
            memcpy(after, d->d_name, strlen(d->d_name) + 1u); /* is_photo() bounds the length */
        }
        if (!lowest[0] || strcmp(d->d_name, lowest) < 0) {
            memcpy(lowest, d->d_name, strlen(d->d_name) + 1u);
        }
    }
    closedir(dir);
    const char *pick = after[0] ? after : lowest[0] ? lowest : NULL;
    s_photos_present = pick != NULL;
    if (!pick) {
        s_media_last[0] = '\0';
        return;
    }
    snprintf(s_media_last, sizeof(s_media_last), "%s", pick);

    char path[128];
    snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%.63s", pick);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > MEDIA_MAX_BYTES) {
        ESP_LOGW(TAG, "photo %s is empty or too large to send; kept on the node", pick);
        return;
    }
    const size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);
    FILE *f = data ? fopen(path, "rb") : NULL;
    const bool read_ok = f && fread(data, 1, size, f) == size;
    if (f) {
        fclose(f);
    }
    if (read_ok) {
        uint8_t digest[32];
        char hex[65], leaf[80];
        fx_sha256(data, size, digest);
        fx_hex_encode(digest, 32, hex);
        snprintf(leaf, sizeof(leaf), "media/%s", hex);
        if (publish(leaf, (const char *)data, size, 1, false) >= 0) {
            memcpy(s_media_pending, hex, sizeof(s_media_pending));
            s_media_waiting = true;
            s_media_sent_ms = now;
            s_media_sent++;
        }
    } else {
        ESP_LOGW(TAG, "photo %s could not be read (%s)", pick, data ? "file error" : "out of memory");
    }
    free(data);
}

/* --- episode traces (M5) ------------------------------------------------------------ */

static bool is_trace(const char *name)
{
    const size_t len = strlen(name);
    return len > 4u && len < sizeof(s_trace_last) && strcmp(name + len - 4u, ".fxt") == 0;
}

/* The gateway holds the trace and checked its hash. Trace names carry no hash, so each is hashed. */
static void on_trace_ack(const char *sha)
{
    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (dir) {
        struct dirent *d;
        char path[128], hex[65];
        uint8_t digest[32];
        while ((d = readdir(dir)) != NULL) {
            if (!is_trace(d->d_name)) {
                continue;
            }
            snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%.63s", d->d_name);
            if (hash_file(path, digest)) {
                fx_hex_encode(digest, 32, hex);
                if (strcmp(hex, sha) == 0 && unlink(path) == 0) {
                    s_traces_deleted++;
                    ESP_LOGI(TAG, "trace %.16s is on the gateway; deleted here", sha);
                    break;
                }
            }
        }
        closedir(dir);
    }
    if (s_trace_waiting && strcmp(sha, s_trace_pending) == 0) {
        s_trace_waiting = false;
    }
}

/* Traces go after photos: one at a time, and only while no photo is waiting to go.
 * The same name-order walk as photos, so one the gateway refuses never blocks the rest. */
static void pump_traces(uint32_t now)
{
    if (!g_node.media_ok || !s_mqtt_up || !s_sync.synced || s_photos_present || s_media_waiting) {
        return;
    }
    if (s_trace_waiting && now - s_trace_sent_ms < MEDIA_ACK_WAIT_MS) {
        return;
    }
    s_trace_waiting = false;

    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (!dir) {
        return;
    }
    char after[64] = "", lowest[64] = "";
    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        if (!is_trace(d->d_name)) {
            continue;
        }
        if (strcmp(d->d_name, s_trace_last) > 0 && (!after[0] || strcmp(d->d_name, after) < 0)) {
            memcpy(after, d->d_name, strlen(d->d_name) + 1u); /* is_trace() bounds the length */
        }
        if (!lowest[0] || strcmp(d->d_name, lowest) < 0) {
            memcpy(lowest, d->d_name, strlen(d->d_name) + 1u);
        }
    }
    closedir(dir);
    const char *pick = after[0] ? after : lowest[0] ? lowest : NULL;
    if (!pick) {
        s_trace_last[0] = '\0';
        return;
    }
    snprintf(s_trace_last, sizeof(s_trace_last), "%s", pick);

    char path[128];
    snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%.63s", pick);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > TRACE_MAX_BYTES) {
        ESP_LOGW(TAG, "trace %s is empty or too large to send; kept on the node", pick);
        return;
    }
    const size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);
    FILE *f = data ? fopen(path, "rb") : NULL;
    const bool read_ok = f && fread(data, 1, size, f) == size;
    if (f) {
        fclose(f);
    }
    if (read_ok) {
        uint8_t digest[32];
        char hex[65], leaf[80];
        fx_sha256(data, size, digest);
        fx_hex_encode(digest, 32, hex);
        snprintf(leaf, sizeof(leaf), "trace/%s", hex);
        if (publish(leaf, (const char *)data, size, 1, false) >= 0) {
            memcpy(s_trace_pending, hex, sizeof(s_trace_pending));
            s_trace_waiting = true;
            s_trace_sent_ms = now;
            s_traces_sent++;
        }
    } else {
        ESP_LOGW(TAG, "trace %s could not be read (%s)", pick, data ? "file error" : "out of memory");
    }
    free(data);
}

/* --- state and telemetry ---------------------------------------------------------- */

static node_state_t read_state(void)
{
    node_state_t s;
    portENTER_CRITICAL(&g_node.state_lock);
    s.have_fusion = g_node.have_fusion;
    s.fusion = g_node.fusion_state;
    s.alarm = g_node.alarm_state;
    s.sensor = g_node.sensor_fault;
    s.ledger = g_node.ledger_fault;
    s.selftest = g_node.selftest_fault;
    s.buzzer = g_node.buzzer_error;
    portEXIT_CRITICAL(&g_node.state_lock);
    return s;
}

static bool same_state(const node_state_t *a, const node_state_t *b)
{
    return a->have_fusion == b->have_fusion && a->fusion == b->fusion && a->alarm == b->alarm && a->sensor == b->sensor &&
           a->ledger == b->ledger && a->selftest == b->selftest && a->buzzer == b->buzzer;
}

static void send_state(const node_state_t *s)
{
    char body[240];
    size_t n = 0;
    if (appendf(body, sizeof(body), &n,
                "{\"fusion\":\"%s\",\"alarm\":\"%s\",\"faults\":{\"sensor\":%s,\"ledger\":%s,\"selftest\":%s,\"buzzer\":%s},"
                "\"uptime_ms\":%" PRIu32 "}",
                s->have_fusion ? fx_state_name(s->fusion) : "WARMUP", fx_alarm_state_name(s->alarm), json_bool(s->sensor),
                json_bool(s->ledger), json_bool(s->selftest), json_bool(s->buzzer), node_now_ms())) {
        publish("state", body, n, 1, true);
    }
}

static void send_telemetry(void)
{
    fx_sample_t samples[FX_MODALITY_COUNT];
    float effective[FX_MODALITY_COUNT];
    portENTER_CRITICAL(&g_node.state_lock);
    const bool have = g_node.have_fusion;
    memcpy(samples, g_node.samples, sizeof(samples));
    memcpy(effective, g_node.effective, sizeof(effective));
    portEXIT_CRITICAL(&g_node.state_lock);
    if (!have) {
        return;
    }

    char body[360];
    size_t n = 0;
    bool ok = appendf(body, sizeof(body), &n, "{\"uptime_ms\":%" PRIu32 ",\"scores\":{", node_now_ms());
    bool first = true;
    for (int m = 0; ok && m < FX_MODALITY_COUNT; m++) {
        if (!samples[m].valid) {
            continue; /* invalid modalities are left out, as the transport defines */
        }
        ok = appendf(body, sizeof(body), &n, "%s\"%s\":%" PRIu32, first ? "" : ",", fx_modality_key((fx_modality_t)m),
                     fx_hundredths(effective[m]));
        first = false;
    }
    ok = ok && appendf(body, sizeof(body), &n, "}");
    if (ok && g_node.fan_ok) {
        const fx_fan_status_t fan = fx_fan_status(&g_node.fan, node_now_ms());
        ok = appendf(body, sizeof(body), &n, ",\"fan\":{\"state\":\"%s\",\"duty\":%u", fx_fan_state_name(fan.state),
                     (unsigned)fan.duty_pct);
        if (ok && fan.tach) {
            ok = appendf(body, sizeof(body), &n, ",\"rpm\":%" PRIu32, fan.rpm);
        }
        ok = ok && appendf(body, sizeof(body), &n, "}");
    }
    if (ok && appendf(body, sizeof(body), &n, "}")) {
        publish("telemetry", body, n, 0, false);
    }
}

static void send_hello(void)
{
    const bounds_t b = ledger_bounds();
    if (!b.ok) {
        /* Without an open ledger a hello would announce next_seq 0 and look like a wiped chain. */
        ESP_LOGW(TAG, "the ledger is not open: state and telemetry only");
        return;
    }
    char partition[16] = "";
    bool pending_verify = false;
    node_ota_describe(partition, sizeof(partition), &pending_verify);
    char body[320];
    size_t n = 0;
    if (appendf(body, sizeof(body), &n,
                "{\"fw_version\":\"%s\",\"boot_count\":%" PRIu32 ",\"next_seq\":%" PRIu64 ",\"oldest_seq\":%" PRIu64
                ",\"uptime_ms\":%" PRIu32 ",\"clock\":\"%s\",\"ota\":{\"partition\":\"%s\",\"pending_verify\":%s}}",
                g_node.fw_version, g_node.boot_count, b.next, b.oldest >= 0 ? (uint64_t)b.oldest : b.next,
                node_now_ms(), g_node.clock.synced ? "gateway" : "uncertain", partition, json_bool(pending_verify))) {
        publish("hello", body, n, 1, false);
    }
}

/* --- MQTT --------------------------------------------------------------------------- */

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        s_connections++;
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        break;
    case MQTT_EVENT_DATA: {
        fx_cmd_t cmd;
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            ESP_LOGW(TAG, "a command split across packets was ignored");
        } else if (!fx_cmd_parse(ev->data, (size_t)ev->data_len, &cmd)) {
            ESP_LOGW(TAG, "a malformed command was ignored");
        } else if (xQueueSend(s_cmds, &cmd, 0) != pdTRUE) {
            ESP_LOGW(TAG, "command queue full: a command was dropped (the next sync recovers)");
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "connection failed: esp-tls 0x%x, TLS -0x%x, socket errno %d",
                     (unsigned)ev->error_handle->esp_tls_last_esp_err, (unsigned)-ev->error_handle->esp_tls_stack_err,
                     ev->error_handle->esp_transport_sock_errno);
        } else if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "the broker refused the connection (code %d)", (int)ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static bool start_mqtt(void)
{
    const char *key = fx_tls_key_pem(&g_node.tls_key);
    if (!key) {
        return false;
    }
    static char uri[FX_NET_HOST_MAX + 16];
    snprintf(uri, sizeof(uri), "mqtts://%s:%u", s_creds.mqtt_host, (unsigned)s_creds.mqtt_port);
    fx_hex_encode(g_node.key.device_id, 16, s_client_id);
    snprintf(s_topic_prefix, sizeof(s_topic_prefix), "firex/%s/%s/", s_creds.site_id, s_client_id);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.certificate = s_creds.ca_pem,
        .credentials.client_id = s_client_id,
        .credentials.authentication.certificate = s_creds.cert_pem,
        .credentials.authentication.key = key,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = (int)MQTT_RETRY_MS,
        .network.timeout_ms = 15000,
        .buffer.size = 4096,
        .task.priority = 2,
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return false;
    }
    esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, on_mqtt, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return false;
    }
    ESP_LOGI(TAG, "connecting to %s as %s", uri, s_client_id);
    return true;
}

static void on_connected(void)
{
    fx_sync_disconnected(&s_sync);
    s_media_waiting = false;
    char topic[168];
    snprintf(topic, sizeof(topic), "%scmd", s_topic_prefix);
    if (esp_mqtt_client_subscribe_single(s_client, topic, 1) < 0) {
        ESP_LOGW(TAG, "could not subscribe to %s", topic);
    }
    send_hello();
    ESP_LOGI(TAG, "connected to the gateway broker");
}

static void handle_cmd(const fx_cmd_t *cmd)
{
    const bounds_t b = ledger_bounds();
    fx_sync_on_cmd(&s_sync, cmd, b.oldest);
    if (cmd->kind == FX_CMD_SYNC || cmd->kind == FX_CMD_ACK) {
        set_acked(s_sync.acked);
    } else if (cmd->kind == FX_CMD_MEDIA_ACK) {
        on_media_ack(cmd->sha256);
    } else if (cmd->kind == FX_CMD_TRACE_ACK) {
        on_trace_ack(cmd->sha256);
    } else if (cmd->kind == FX_CMD_TIME) {
        node_clock_sync((uint64_t)cmd->number);
    } else if (cmd->kind == FX_CMD_OTA) {
        const char *why = NULL;
        if (!node_ota_request(cmd, &why)) {
            ESP_LOGW(TAG, "firmware %s offered and refused: %s", cmd->version, why ? why : "?");
        }
    }
}

/* --- provisioning and the task ------------------------------------------------------- */

static void try_provision(void)
{
    char host[FX_NET_HOST_MAX];
    uint16_t port = 0;
    if (fx_net_gateway_get(host, &port) != ESP_OK) {
        ESP_LOGW(TAG, "no gateway set: use `gateway set <host>` or the provisioning app");
        return;
    }
    const fx_prov_client_t client = {
        .host = host,
        .port = port,
        .code = g_node.pairing_code,
        .fw_version = g_node.fw_version,
        .device_id = g_node.key.device_id,
        .ledger_pub = g_node.key.pub,
        .ledger_signer = fx_device_key_signer(&g_node.key),
        .tls = &g_node.tls_key,
    };
    fx_net_creds_t creds;
    int status = 0;
    const fx_prov_outcome_t outcome = fx_prov_run(&client, &creds, &status);
    s_provision_outcome = outcome;
    s_provision_tried = true;
    if (outcome != FX_PROV_OK) {
        ESP_LOGW(TAG, "provisioning with %s port %u: %s", host, (unsigned)port, fx_prov_outcome_text(outcome));
        return;
    }
    if (fx_net_creds_store(&creds) != ESP_OK) {
        ESP_LOGE(TAG, "provisioned, but the credentials could not be stored: this boot only");
    }
    s_creds = creds;
    s_have_creds = true;
    ESP_LOGI(TAG, "provisioned into site %s; broker %s port %u", s_creds.site_id, s_creds.mqtt_host,
             (unsigned)s_creds.mqtt_port);
}

static void forget(bool gateway)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_mqtt_up = false;
    s_have_creds = false;
    fx_net_creds_free(&s_creds);
    fx_sync_disconnected(&s_sync);
    set_acked(-1);
    const esp_err_t err = fx_net_creds_erase(gateway);
    ESP_LOGW(TAG, "site credentials%s forgotten%s", gateway ? " and gateway address" : "",
             err == ESP_OK ? "" : " in memory, but NVS could not be updated");
}

static void net_task(void *arg)
{
    (void)arg;
    fx_sync_init(&s_sync, SEND_WINDOW);
    s_have_creds = fx_net_creds_load(&s_creds) == ESP_OK;

    uint32_t last_provision = 0, last_mqtt_try = 0, last_state = 0, last_telemetry = 0;
    bool provision_tried = false, mqtt_tried = false, state_sent = false;
    uint32_t seen_connections = 0;
    node_state_t sent_state;
    memset(&sent_state, 0, sizeof(sent_state));
    fx_cmd_t cmd;

    for (;;) {
        const uint32_t now = node_now_ms();

        if (s_forget_requested) {
            s_forget_requested = false;
            forget(s_forget_gateway);
            provision_tried = false;
        }

        if (!s_have_creds) {
            const bool due = s_provision_requested || !provision_tried || now - last_provision >= PROVISION_RETRY_MS;
            if (fx_wifi_connected() && g_node.key_ok && due) {
                s_provision_requested = false;
                provision_tried = true;
                last_provision = now;
                try_provision();
            }
        } else if (!s_client && fx_wifi_connected() && (!mqtt_tried || now - last_mqtt_try >= MQTT_RETRY_MS)) {
            mqtt_tried = true;
            last_mqtt_try = now;
            if (!start_mqtt()) {
                ESP_LOGE(TAG, "the MQTT client did not start");
            }
        }

        if (s_client && s_mqtt_up) {
            if (s_connections != seen_connections) {
                seen_connections = s_connections;
                on_connected();
                state_sent = false;
            }
            const node_state_t st = read_state();
            if (!state_sent || !same_state(&st, &sent_state) || now - last_state >= STATE_MS) {
                send_state(&st);
                sent_state = st;
                state_sent = true;
                last_state = now;
            }
            if (now - last_telemetry >= TELEMETRY_MS) {
                send_telemetry();
                last_telemetry = now;
            }
            pump_ledger();
            pump_media(now);
            pump_traces(now);
        } else if (s_sync.synced) {
            fx_sync_disconnected(&s_sync);
        }

        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(LOOP_MS)) == pdTRUE) {
            do {
                handle_cmd(&cmd);
            } while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE);
        }
    }
}

void node_net_start(void)
{
    if (!g_node.key_ok || !g_node.tls_ok || !g_node.pairing_ok) {
        ESP_LOGE(TAG, "network not started: the unit needs its device key, TLS key and pairing code");
        return;
    }
    /* Both strings must outlive onboarding: the provisioning manager keeps the pointers. */
    static char pop[12], service[16];
    char id[33];
    fx_pairing_code_format(g_node.pairing_code, pop);
    fx_hex_encode(g_node.key.device_id, 16, id);
    snprintf(service, sizeof(service), "FIREX-%.6s", id);
    for (char *p = service; *p; p++) {
        if (*p >= 'a' && *p <= 'f') {
            *p = (char)(*p - 'a' + 'A');
        }
    }

    s_cmds = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(fx_cmd_t));
    if (!s_cmds) {
        ESP_LOGE(TAG, "network not started: no memory for the command queue");
        return;
    }
    const esp_err_t err = fx_wifi_start(pop, service, (uint16_t)CONFIG_FIREX_GATEWAY_PROVISION_PORT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi did not start (%s); the alarm is unaffected", esp_err_to_name(err));
        return;
    }
    if (fx_wifi_onboarding()) {
        printf("onboarding: open the ESP SoftAP Provisioning app, join %s, proof of possession %s\n", service, pop);
    }
    xTaskCreate(net_task, "net", 8192, NULL, 2, NULL);
}

void node_net_print_status(void)
{
    char line[160];
    fx_wifi_describe(line, sizeof(line));
    printf("wifi     %s\n", line);
    char host[FX_NET_HOST_MAX];
    uint16_t port = 0;
    if (fx_net_gateway_get(host, &port) == ESP_OK) {
        printf("gateway  %s port %u\n", host, (unsigned)port);
    } else {
        printf("gateway  not set\n");
    }
    if (s_have_creds) {
        printf("site     %s, broker %s port %u, %s\n", s_creds.site_id, s_creds.mqtt_host, (unsigned)s_creds.mqtt_port,
               s_mqtt_up ? "connected" : "not connected");
    } else {
        printf("site     not provisioned%s%s\n", s_provision_tried ? ": " : "",
               s_provision_tried ? fx_prov_outcome_text(s_provision_outcome) : "");
    }
    printf("ledger   gateway holds up to %" PRId64 ", next to send %" PRId64 "%s, %" PRIu32 " sent this boot\n",
           node_net_acked(), s_sync.next, s_sync.synced ? "" : " (waiting for sync)", s_entries_sent);
    printf("photos   %" PRIu32 " sent, %" PRIu32 " acknowledged and deleted%s\n", s_media_sent, s_media_deleted,
           s_media_waiting ? ", one waiting for the gateway" : "");
    printf("traces   %" PRIu32 " sent, %" PRIu32 " acknowledged and deleted%s\n", s_traces_sent, s_traces_deleted,
           s_trace_waiting ? ", one waiting for the gateway" : "");
    printf("clock    %s\n", g_node.clock.synced ? "set by the gateway on this boot" : "not set by the gateway on this boot");
}

void node_net_request_provision(void)
{
    s_provision_requested = true;
}

void node_net_request_forget(bool gateway)
{
    s_forget_gateway = gateway;
    s_forget_requested = true;
}

#else /* CONFIG_FIREX_NET_ENABLE */

void node_net_start(void)
{
    ESP_LOGI(TAG, "the network is disabled in this build");
}

void node_net_print_status(void)
{
    printf("the network is disabled in this build (FIREX_NET_ENABLE)\n");
}

void node_net_request_provision(void) {}

void node_net_request_forget(bool gateway)
{
    (void)gateway;
}

int64_t node_net_acked(void)
{
    return -1;
}

bool node_net_synced(void)
{
    return false;
}

const fx_net_creds_t *node_net_creds(void)
{
    return NULL;
}

#endif /* CONFIG_FIREX_NET_ENABLE */
