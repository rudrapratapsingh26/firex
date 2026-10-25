/*
 * The trace task (M5). Every 500 ms the scores fusion was fed, its state, the
 * alarm and the fan go into the trace component's pre-window, and during an
 * episode into a file on the media partition (components/trace). A closed file
 * is hashed, renamed to t<boot>-<n>.fxt and written to the ledger as
 * trace_captured; the network task sends it to the gateway and deletes it once
 * the gateway has it.
 *
 * Photos come first: an episode is dropped rather than started without room for
 * a whole trace and a capture's frames, and a file that starts eating into the
 * frames' room is abandoned. The task runs at the lowest application priority
 * and nothing waits on it. It samples shared state on its own schedule, so a
 * trace sample may repeat or skip one fusion cycle.
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "fx_sha256.h"
#include "fx_trace.h"
#include "node.h"

#define PHOTO_RESERVE_BYTES (512u * 1024u)
#define TRACE_MAX_BYTES (FX_TRACE_HEADER_LEN + FX_TRACE_MAX_SAMPLES * FX_TRACE_SAMPLE_LEN)
#define SPACE_CHECK_SAMPLES 20u

static const char *TAG = "trace";

typedef struct {
    FILE *f;
    char tmp[64];
    uint32_t writes;
} file_ctx_t;

static fx_trace_sample_t s_ring[FX_TRACE_PRE_SAMPLES];
static fx_trace_t s_trace;
static file_ctx_t s_file;
static uint32_t s_files, s_closed, s_dropped, s_abandoned;
static char s_last_name[32];
static char s_last_sha[65];

static bool room_for(size_t bytes)
{
    size_t total = 0, used = 0;
    return g_node.media_ok && fx_fs_usage("media", &total, &used) == ESP_OK && total > used &&
           total - used >= bytes + PHOTO_RESERVE_BYTES;
}

static bool file_begin(void *ctx, const fx_trace_header_t *h)
{
    file_ctx_t *c = (file_ctx_t *)ctx;
    if (!room_for(TRACE_MAX_BYTES)) {
        return false;
    }
    snprintf(c->tmp, sizeof(c->tmp), NODE_MEDIA_BASE "/t%" PRIu32 "-%" PRIu32 ".tmp", g_node.boot_count, s_files + 1u);
    c->f = fopen(c->tmp, "wb");
    c->writes = 0;
    uint8_t header[FX_TRACE_HEADER_LEN];
    fx_trace_header_encode(h, header);
    if (!c->f || fwrite(header, 1, sizeof(header), c->f) != sizeof(header)) {
        if (c->f) {
            fclose(c->f);
            c->f = NULL;
            unlink(c->tmp);
        }
        return false;
    }
    s_files++;
    return true;
}

static bool file_write(void *ctx, const uint8_t sample[FX_TRACE_SAMPLE_LEN])
{
    file_ctx_t *c = (file_ctx_t *)ctx;
    if (++c->writes % SPACE_CHECK_SAMPLES == 0u && !room_for(0)) {
        return false; /* the frames' reserve is the limit */
    }
    return c->f && fwrite(sample, 1, FX_TRACE_SAMPLE_LEN, c->f) == FX_TRACE_SAMPLE_LEN;
}

static void file_abort(void *ctx)
{
    file_ctx_t *c = (file_ctx_t *)ctx;
    if (c->f) {
        fclose(c->f);
        c->f = NULL;
    }
    unlink(c->tmp);
}

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

/* The final header over the placeholder, then the hash, the name and the ledger entry. */
static void file_end(void *ctx, const fx_trace_header_t *h)
{
    file_ctx_t *c = (file_ctx_t *)ctx;
    uint8_t header[FX_TRACE_HEADER_LEN];
    fx_trace_header_encode(h, header);
    bool ok = c->f && fseek(c->f, 0, SEEK_SET) == 0 && fwrite(header, 1, sizeof(header), c->f) == sizeof(header);
    if (c->f) {
        ok = fclose(c->f) == 0 && ok;
        c->f = NULL;
    }

    uint8_t digest[32];
    char name[32], path[64];
    snprintf(name, sizeof(name), "t%" PRIu32 "-%" PRIu32 ".fxt", h->boot_count, s_files);
    snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%s", name);
    if (!ok || !hash_file(c->tmp, digest) || rename(c->tmp, path) != 0) {
        unlink(c->tmp);
        s_abandoned++;
        ESP_LOGW(TAG, "trace %s could not be completed; removed", name);
        return;
    }

    uint8_t payload[FX_PAYLOAD_MAX];
    node_log("trace_captured", payload,
             fx_ev_trace_captured(payload, (fx_state_t)h->trigger, digest, h->samples, h->period_ms, h->pre_samples));
    s_closed++;
    snprintf(s_last_name, sizeof(s_last_name), "%s", name);
    fx_hex_encode(digest, 32, s_last_sha);
    ESP_LOGI(TAG, "%s: %" PRIu32 " samples, %" PRIu32 " before the trigger, sha256 %.16s", name, h->samples,
             h->pre_samples, s_last_sha);
}

/* A trace left open by a reset is incomplete: it never reached the ledger, so it goes. */
static void remove_leftovers(void)
{
    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (!dir) {
        return;
    }
    struct dirent *d;
    char path[128];
    while ((d = readdir(dir)) != NULL) {
        const size_t len = strlen(d->d_name);
        if (d->d_name[0] == 't' && len > 4u && strcmp(d->d_name + len - 4u, ".tmp") == 0) {
            snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%.63s", d->d_name);
            unlink(path);
        }
    }
    closedir(dir);
}

static uint8_t fan_state(fx_fan_state_t s)
{
    switch (s) {
    case FX_FAN_ON: return FX_TRACE_FAN_ON;
    case FX_FAN_FAILED: return FX_TRACE_FAN_FAILED;
    default: return FX_TRACE_FAN_OFF;
    }
}

static void trace_task(void *arg)
{
    (void)arg;
    fx_trace_init(&s_trace, NULL, s_ring, FX_TRACE_PRE_SAMPLES, g_node.boot_count);
    if (g_node.media_ok) {
        remove_leftovers();
    }
    const fx_trace_writer_t writer = {&s_file, file_begin, file_write, file_end, file_abort};

    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(FX_TRACE_PERIOD_MS));

        fx_sample_t samples[FX_MODALITY_COUNT];
        portENTER_CRITICAL(&g_node.state_lock);
        const bool have = g_node.have_fusion;
        memcpy(samples, g_node.samples, sizeof(samples));
        const fx_state_t fusion = g_node.fusion_state;
        const fx_alarm_state_t alarm = g_node.alarm_state;
        portEXIT_CRITICAL(&g_node.state_lock);
        if (!have) {
            continue;
        }

        const uint32_t now = node_now_ms();
        fx_trace_sample_t s;
        memset(&s, 0, sizeof(s));
        for (int m = 0; m < FX_MODALITY_COUNT; m++) {
            s.score[m] = fx_trace_score(samples[m].score, samples[m].valid);
        }
        s.state = (uint8_t)fusion;
        s.alarm = (uint8_t)alarm;
        if (g_node.fan_ok) {
            const fx_fan_status_t fan = fx_fan_status(&g_node.fan, now);
            s.fan_duty = fan.duty_pct;
            s.fan_state = fan_state(fan.state);
        } else {
            s.fan_state = FX_TRACE_FAN_ABSENT;
        }

        switch (fx_trace_feed(&s_trace, &s, now, &writer)) {
        case FX_TRACE_STARTED:
            ESP_LOGI(TAG, "episode: %s, recording with %" PRIu32 " s before it", fx_state_name(fusion),
                     s_trace.header.pre_samples * FX_TRACE_PERIOD_MS / 1000u);
            break;
        case FX_TRACE_DROPPED:
            s_dropped++;
            ESP_LOGW(TAG, "episode: %s, not recorded (media partition short of space or not mounted)",
                     fx_state_name(fusion));
            break;
        case FX_TRACE_ABANDONED:
            s_abandoned++;
            ESP_LOGW(TAG, "trace abandoned: the media partition is needed for photos");
            break;
        default:
            break;
        }
    }
}

void node_trace_start(void)
{
    xTaskCreate(trace_task, "trace", 6144, NULL, 1, NULL);
}

void node_trace_print_status(void)
{
    int pending = 0;
    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (dir) {
        struct dirent *d;
        while ((d = readdir(dir)) != NULL) {
            const size_t len = strlen(d->d_name);
            pending += len > 4u && strcmp(d->d_name + len - 4u, ".fxt") == 0;
        }
        closedir(dir);
    }
    printf("trace    %s, %" PRIu32 " files closed, %" PRIu32 " episodes dropped, %" PRIu32 " abandoned this boot\n",
           s_trace.writing ? "recording" : s_trace.in_episode ? "following an episode without a file" : "idle",
           s_closed, s_dropped, s_abandoned);
    printf("         %d on the media partition waiting for the gateway%s%s%s\n", pending, s_last_name[0] ? "; last " : "",
           s_last_name, s_last_name[0] ? "" : "");
    if (s_last_sha[0]) {
        printf("         last sha256 %s\n", s_last_sha);
    }
}
