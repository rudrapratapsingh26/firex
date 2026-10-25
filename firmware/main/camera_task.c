/*
 * The camera task. It sleeps until fusion signals the transition into
 * CRITICAL, then asks the ESP32-CAM for a fixed number of frames, writes each
 * to the media partition while hashing it, and records the hashes in a
 * media_captured ledger entry. Then it disarms until the next transition.
 *
 * Media policy, as enforced here:
 *   - capture only on the CRITICAL transition; no other code path sends SNAP
 *   - at most cam_frames frames per incident, and at most one incident a minute; a
 *     production image fixes the count at build time (CONFIG_FIREX_CAM_FRAMES_LOCKED)
 *   - a production image has no serial export of photos: they leave only to the gateway
 *   - lowest useful priority; frames are dropped (flash full, camera silent)
 *     rather than anything waiting on them
 *
 * Serial media export (for tools/ledger/export_log.py):
 *   M,file,<name>,<size>,<sha256>      from `media list`
 *   M,begin,<name>,<size> / M,data,<hex> ... / M,end,<sha256>   from `media get`
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "fx_sha256.h"
#include "node.h"

#define MIN_FREE_BYTES (256u * 1024u)
#define INCIDENT_SPACING_MS 60000u
#define FRAME_TIMEOUT_MS 10000u

static const char *TAG = "camera";

typedef struct {
    FILE *f;
    bool write_error;
} sink_t;

static void file_sink(void *ctx, const uint8_t *data, size_t len)
{
    sink_t *s = (sink_t *)ctx;
    if (s->f && fwrite(data, 1, len, s->f) != len) {
        s->write_error = true;
    }
}

static void camera_task(void *arg)
{
    (void)arg;
    static uint8_t payload[FX_PAYLOAD_MAX];
    bool captured_before = false;
    uint32_t last_capture_ms = 0;

    if (g_node.cam_ok && !fx_cam_ping(&g_node.cam, 1500)) {
        ESP_LOGW(TAG, "the camera did not answer PING; captures will be attempted anyway");
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if CONFIG_FIREX_PRODUCTION
        const uint32_t frames = CONFIG_FIREX_CAM_FRAMES_LOCKED; /* media policy: fixed in firmware */
#else
        const uint32_t frames = node_settings_snapshot(NULL).v[FX_S_CAM_FRAMES];
#endif
        const uint32_t now = node_now_ms();
        if (frames == 0 || (captured_before && now - last_capture_ms < INCIDENT_SPACING_MS)) {
            continue;
        }
        captured_before = true;
        last_capture_ms = now;
        g_node.captures++;

        uint8_t hashes[5][32];
        size_t got = 0;
        uint32_t width = 0, height = 0;
        for (uint32_t i = 0; i < frames && i < 5u && g_node.cam_ok && g_node.media_ok; i++) {
            size_t total = 0, used = 0;
            if (fx_fs_usage("media", &total, &used) != ESP_OK || total - used < MIN_FREE_BYTES) {
                ESP_LOGW(TAG, "media partition full: frame %" PRIu32 " dropped", i);
                break;
            }
            const char *tmp = NODE_MEDIA_BASE "/capture.tmp";
            sink_t sink = {fopen(tmp, "wb"), false};
            const fx_cam_result_t res = fx_cam_snap(&g_node.cam, "vga", 12, file_sink, &sink, FRAME_TIMEOUT_MS);
            if (sink.f) {
                fclose(sink.f);
            }
            if (res.status == FX_FRAME_JPEG && sink.f && !sink.write_error) {
                char hex[65], name[96];
                fx_hex_encode(res.sha256, 32, hex);
                snprintf(name, sizeof(name), NODE_MEDIA_BASE "/b%" PRIu32 "-c%" PRIu32 "-f%" PRIu32 "-%.16s.jpg",
                         g_node.boot_count, g_node.captures, i, hex);
                if (rename(tmp, name) == 0) {
                    memcpy(hashes[got++], res.sha256, 32);
                    width = res.width;
                    height = res.height;
                } else {
                    unlink(tmp);
                }
            } else {
                unlink(tmp);
                ESP_LOGW(TAG, "frame %" PRIu32 ": %s", i, res.error[0] ? res.error : "write failed");
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        char resolution[24];
        if (width && height) {
            snprintf(resolution, sizeof(resolution), "%" PRIu32 "x%" PRIu32, width, height);
        } else {
            snprintf(resolution, sizeof(resolution), "none");
        }
        /* Written even with zero frames: an attempted capture that produced
         * nothing is itself part of the record. */
        const size_t len = fx_ev_media_captured(payload, (const uint8_t (*)[32])hashes, got, NULL, "jpeg", resolution);
        node_log("media_captured", payload, len);
    }
}

void node_camera_start(void)
{
    xTaskCreate(camera_task, "camera", 6144, NULL, 3, &g_node.camera_task);
}

#if !CONFIG_FIREX_PRODUCTION
static bool safe_name(const char *name)
{
    return name && name[0] && !strchr(name, '/') && !strstr(name, "..") && strlen(name) < 64;
}
#endif

static bool hash_file(const char *path, uint8_t digest[32], long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    uint8_t buf[1024];
    size_t n;
    long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fx_sha256_update(&ctx, buf, n);
        total += (long)n;
    }
    fclose(f);
    fx_sha256_final(&ctx, digest);
    *size = total;
    return true;
}

void node_media_list(void)
{
    DIR *dir = opendir(NODE_MEDIA_BASE);
    if (!dir) {
        printf("media partition not mounted\n");
        return;
    }
    struct dirent *e;
    int count = 0;
    while ((e = readdir(dir)) != NULL) {
        const size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".jpg") != 0) {
            continue;
        }
        char path[128], hex[65];
        uint8_t digest[32];
        long size = 0;
        snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%.96s", e->d_name);
        if (hash_file(path, digest, &size)) {
            fx_hex_encode(digest, 32, hex);
            printf("M,file,%s,%ld,%s\n", e->d_name, size, hex);
            count++;
        }
    }
    closedir(dir);
    printf("M,count,%d\n", count);
    fflush(stdout);
}

#if !CONFIG_FIREX_PRODUCTION
void node_media_get(const char *name)
{
    if (!safe_name(name)) {
        printf("M,error,bad name\n");
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), NODE_MEDIA_BASE "/%s", name);
    struct stat st;
    FILE *f = stat(path, &st) == 0 ? fopen(path, "rb") : NULL;
    if (!f) {
        printf("M,error,no such file\n");
        return;
    }
    printf("M,begin,%s,%ld\n", name, (long)st.st_size);
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    uint8_t buf[256];
    char hex[2 * sizeof(buf) + 1];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fx_sha256_update(&ctx, buf, n);
        fx_hex_encode(buf, n, hex);
        printf("M,data,%s\n", hex);
    }
    fclose(f);
    uint8_t digest[32];
    fx_sha256_final(&ctx, digest);
    fx_hex_encode(digest, 32, hex);
    printf("M,end,%s\n", hex);
    fflush(stdout);
}
#endif /* !CONFIG_FIREX_PRODUCTION */
