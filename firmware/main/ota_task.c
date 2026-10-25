/*
 * Over-the-air updates (M5), carried out. The decisions are pure C in
 * components/ota; this file is the ESP-IDF work around them.
 *
 *   node_ota_request  the network task passes on an `ota` command. A refusal is a
 *                     fw_update entry (phase failed), so the gateway's rollout sees it.
 *   the OTA task      lowest priority. It streams the image over HTTPS, with the
 *                     unit's own client certificate, into the other slot, hashing as it
 *                     goes. The first bytes must name this project, the offered version
 *                     and this image's kind (devkit, evt, evt-production, M6). esp_ota_end
 *                     checks the RSA signature block against the key in the running image.
 *                     Then it waits for a quiet minute and reboots.
 *   confirm           a new image proves itself within FX_OTA_CONFIRM_MS or rolls back
 *   rolled_back       the old image, running again, records what happened, once
 *
 * The alarm never waits on any of it: this task runs below every other, and the
 * reboot waits until fusion has been NORMAL for a minute with the alarm quiet.
 *
 * NVS namespace fx_ota: the image being installed (so the new image, or the old one
 * after a rollback, can name it), why a new image gave up, and which sensors the
 * last confirmed image had ready.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "fx_sha256.h"
#include "node.h"
#include "nvs.h"

static const char *TAG = "ota";

#define NVS_NS "fx_ota"
#define TASK_PRIORITY 1
#define CHUNK 4096
#define CHECK_MS 1000u
#define DRAIN_WAIT_MS 3000u
#define HTTP_TIMEOUT_MS 15000

_Static_assert(sizeof(((fx_cmd_t *)0)->version) == FX_OTA_VERSION_MAX, "ota command version size");
_Static_assert(sizeof(((fx_cmd_t *)0)->url) == FX_OTA_URL_MAX, "ota command url size");
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) ==
                   FX_OTA_KIND_OFFSET,
               "the custom app description follows esp_app_desc_t");
_Static_assert(sizeof(fx_ota_image_kind_t) == 32u, "image kind record size");

typedef struct {
    char version[FX_OTA_VERSION_MAX];
    char sha256[65];
    char url[FX_OTA_URL_MAX];
    uint32_t size;
} offer_t;

static QueueHandle_t s_offers;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static bool s_pending_verify;
static uint32_t s_received; /* bytes of the image being downloaded */
static char s_status[120] = "no update this boot";

/* --- small helpers ------------------------------------------------------------- */

static void set_status(const char *fmt, ...)
{
    char text[sizeof(s_status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status, text, sizeof(s_status));
    portEXIT_CRITICAL(&s_lock);
}

static void log_update(fx_ota_phase_t phase, const char *version, const char *from, const uint8_t *image,
                       const char *reason)
{
    uint8_t payload[FX_PAYLOAD_MAX];
    const size_t len = fx_ev_fw_update(payload, phase, version, from, image, reason);
    if (len == 0 || !node_log("fw_update", payload, len)) {
        ESP_LOGE(TAG, "the fw_update entry (%s) was not queued", fx_ota_phase_name(phase));
    }
}

/* Entries queued before a restart reach flash first. */
static void wait_for_ledger(void)
{
    for (uint32_t waited = 0; waited < DRAIN_WAIT_MS && g_node.ledger_queue &&
                              uxQueueMessagesWaiting(g_node.ledger_queue) > 0;
         waited += 50u) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(250)); /* the entry the ledger task took last is still being written */
}

static bool nvs_get_text(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    const bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void nvs_put_text(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (value) {
            nvs_set_str(h, key, value);
        } else {
            nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_get_byte(const char *key, uint8_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_get_u8(h, key, out) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void nvs_put_byte(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t sensors_ready(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < NODE_SENSOR_COUNT; i++) {
        if (g_node.sensors[i] && g_node.sensors[i]->ready) {
            mask |= (uint8_t)(1u << i);
        }
    }
    return mask;
}

/* The image being installed, as the download recorded it, if it is `version`. */
static bool installing(const char *version, char *from, size_t from_cap, uint8_t image[32])
{
    char ver[FX_OTA_VERSION_MAX], sha[65];
    if (!nvs_get_text("ver", ver, sizeof(ver)) || strncmp(ver, version, sizeof(ver)) != 0) {
        return false;
    }
    if (!nvs_get_text("from", from, from_cap)) {
        from[0] = '\0';
    }
    return nvs_get_text("sha", sha, sizeof(sha)) && fx_ota_parse_sha256(sha, image);
}

/* --- the command ----------------------------------------------------------------- */

bool node_ota_request(const fx_cmd_t *cmd, const char **why)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

    fx_ota_node_t node;
    memset(&node, 0, sizeof(node));
    portENTER_CRITICAL(&g_node.state_lock);
    node.have_fusion = g_node.have_fusion;
    node.fusion = g_node.fusion_state;
    node.self_test_running = g_node.self_test_running;
    node.injecting = g_node.injecting;
    portEXIT_CRITICAL(&g_node.state_lock);
    portENTER_CRITICAL(&s_lock);
    node.update_in_progress = s_busy;
    node.pending_verify = s_pending_verify;
    portEXIT_CRITICAL(&s_lock);
    node.running_version = app->version;

    const fx_ota_offer_t offer = {cmd->version, cmd->size, cmd->sha256, cmd->url};
    const char *refusal = !s_offers  ? "the OTA task is not running"
                          : !target ? "this image has no partition to update into"
                                    : fx_ota_refusal(&node, &offer, (uint32_t)target->size);
    if (!refusal) {
        portENTER_CRITICAL(&s_lock);
        if (s_busy) {
            refusal = "an update is already in progress";
        } else {
            s_busy = true;
        }
        portEXIT_CRITICAL(&s_lock);
    }

    uint8_t image[32];
    const bool have_image = fx_ota_parse_sha256(cmd->sha256, image);
    if (refusal) {
        char reason[112];
        snprintf(reason, sizeof(reason), "refused: %s", refusal);
        log_update(FX_OTA_FAILED, cmd->version, app->version, have_image ? image : NULL, reason);
        ESP_LOGW(TAG, "update to %.31s %s", cmd->version, reason);
        if (why) {
            *why = refusal;
        }
        return false;
    }

    offer_t o;
    memset(&o, 0, sizeof(o));
    memcpy(o.version, cmd->version, sizeof(o.version));
    o.version[sizeof(o.version) - 1] = '\0';
    memcpy(o.sha256, cmd->sha256, sizeof(o.sha256));
    o.sha256[sizeof(o.sha256) - 1] = '\0';
    memcpy(o.url, cmd->url, sizeof(o.url));
    o.url[sizeof(o.url) - 1] = '\0';
    o.size = cmd->size;
    if (xQueueSend(s_offers, &o, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
        log_update(FX_OTA_FAILED, o.version, app->version, image, "refused: the OTA task is busy");
        if (why) {
            *why = "the OTA task is busy";
        }
        return false;
    }
    log_update(FX_OTA_ACCEPTED, o.version, app->version, image, NULL);
    set_status("%s accepted", o.version);
    return true;
}

/* --- the download ------------------------------------------------------------------- */

/* Checks the app description at the head of the image before any more is written. */
static const char *check_head(const uint8_t *head, const char *version)
{
    esp_app_desc_t desc;
    memcpy(&desc, head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
    if (head[0] != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "the file is not an ESP-IDF app image";
    }
    if (strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        return "the image is for another project";
    }
    if (strncmp(desc.version, version, sizeof(desc.version)) != 0) {
        return "the image's own version differs from the command's";
    }
    fx_ota_image_kind_t kind;
    memcpy(&kind, head + FX_OTA_KIND_OFFSET, sizeof(kind));
    return fx_ota_kind_refusal(&kind, fx_image_kind.kind);
}

static const char *download(const offer_t *o, const esp_partition_t *target, uint8_t digest[32])
{
    const fx_net_creds_t *creds = node_net_creds();
    const char *key = fx_tls_key_pem(&g_node.tls_key);
    if (!creds || !creds->ca_pem || !creds->cert_pem || !key) {
        return "the unit is not provisioned";
    }
    const esp_http_client_config_t http = {
        .url = o->url,
        .cert_pem = creds->ca_pem,
        .client_cert_pem = creds->cert_pem,
        .client_key_pem = key,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = CHUNK,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        return "no memory for the HTTP client";
    }

    enum { HEAD_LEN = FX_OTA_KIND_OFFSET + sizeof(fx_ota_image_kind_t) };
    uint8_t head[HEAD_LEN];
    size_t head_len = 0;
    const char *err = NULL;
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    uint32_t total = 0;
    fx_sha256_ctx ctx;
    fx_sha256_init(&ctx);
    char *buf = malloc(CHUNK);

    if (!buf) {
        err = "no memory for the download";
    } else if (esp_http_client_open(client, 0) != ESP_OK) {
        err = "the gateway did not answer";
    } else {
        const int64_t length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status == 403) {
            err = "the gateway refused this unit's certificate";
        } else if (status == 404) {
            err = "the gateway does not have that image";
        } else if (status != 200) {
            err = "the gateway answered with an error";
        } else if (length > 0 && (uint64_t)length != o->size) {
            err = "the image size differs from the command's";
        } else if (esp_ota_begin(target, o->size, &ota) != ESP_OK) {
            err = "the update partition could not be prepared";
        } else {
            ota_open = true;
            for (;;) {
                const int n = esp_http_client_read(client, buf, CHUNK);
                if (n < 0) {
                    err = "the download broke off";
                    break;
                }
                if (n == 0) {
                    break;
                }
                if (total + (uint32_t)n > o->size) {
                    err = "the image is longer than the command said";
                    break;
                }
                if (head_len < HEAD_LEN) {
                    const size_t take = (size_t)n < HEAD_LEN - head_len ? (size_t)n : HEAD_LEN - head_len;
                    memcpy(head + head_len, buf, take);
                    head_len += take;
                    if (head_len == HEAD_LEN && (err = check_head(head, o->version)) != NULL) {
                        break;
                    }
                }
                if (esp_ota_write(ota, buf, (size_t)n) != ESP_OK) {
                    err = "writing the update partition failed";
                    break;
                }
                fx_sha256_update(&ctx, (const uint8_t *)buf, (size_t)n);
                total += (uint32_t)n;
                portENTER_CRITICAL(&s_lock);
                s_received = total;
                portEXIT_CRITICAL(&s_lock);
            }
            if (!err && (total != o->size || head_len < HEAD_LEN)) {
                err = "the download ended early";
            }
        }
    }

    if (!err) {
        uint8_t want[32];
        fx_sha256_final(&ctx, digest);
        if (!fx_ota_parse_sha256(o->sha256, want) || memcmp(digest, want, 32) != 0) {
            err = "the image's SHA-256 differs from the command's";
        }
    }
    if (ota_open) {
        if (err) {
            esp_ota_abort(ota);
        } else {
            /* Validates the image and, with signed apps, its signature block. */
            const esp_err_t end = esp_ota_end(ota);
            if (end == ESP_ERR_OTA_VALIDATE_FAILED) {
                err = "the image failed validation: damaged, or not signed with this unit's release key";
            } else if (end != ESP_OK) {
                err = "the image could not be finished";
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    return err;
}

static bool quiet_now(fx_ota_quiet_t *q)
{
    fx_ota_quiet_input_t in;
    portENTER_CRITICAL(&g_node.state_lock);
    in.have_fusion = g_node.have_fusion;
    in.fusion = g_node.fusion_state;
    in.alarm_active = g_node.alarm_state == FX_ALARM_CRITICAL || g_node.alarm_state == FX_ALARM_SILENCED;
    in.busy = g_node.self_test_running || g_node.injecting;
    portEXIT_CRITICAL(&g_node.state_lock);
    in.now_ms = node_now_ms();
    return fx_ota_quiet_step(q, &in, FX_OTA_QUIET_MS);
}

static void run_update(const offer_t *o)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    uint8_t want[32], digest[32];
    fx_ota_parse_sha256(o->sha256, want);

    set_status("%s downloading", o->version);
    ESP_LOGI(TAG, "downloading %s into %s", o->version, target ? target->label : "?");
    const char *err = target ? download(o, target, digest) : "this image has no partition to update into";
    if (err) {
        log_update(FX_OTA_FAILED, o->version, app->version, want, err);
        set_status("%s failed: %s", o->version, err);
        ESP_LOGW(TAG, "%s: %s", o->version, err);
        return;
    }

    nvs_put_text("ver", o->version);
    nvs_put_text("sha", o->sha256);
    nvs_put_text("from", app->version);
    nvs_put_text("why", NULL);
    log_update(FX_OTA_DOWNLOADED, o->version, app->version, digest, NULL);
    set_status("%s downloaded; restarting after a quiet minute", o->version);
    ESP_LOGI(TAG, "%s downloaded and verified; waiting for fusion to be NORMAL for a minute", o->version);

    /* Quiet for a minute, the ledger flushed, and still quiet after that. */
    fx_ota_quiet_t q;
    fx_ota_quiet_init(&q);
    for (;;) {
        if (quiet_now(&q)) {
            wait_for_ledger();
            if (quiet_now(&q)) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        log_update(FX_OTA_FAILED, o->version, app->version, digest, "the new image could not be made the boot image");
        set_status("%s failed: could not be made the boot image", o->version);
        return;
    }
    ESP_LOGW(TAG, "restarting into %s", o->version);
    esp_restart();
}

/* --- after the restart ---------------------------------------------------------------- */

static void confirm_or_roll_back(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char from[FX_OTA_VERSION_MAX] = "";
    uint8_t image[32];
    const bool have_image = installing(app->version, from, sizeof(from), image);
    uint8_t expected = 0;
    nvs_get_byte("sensors", &expected); /* none stored: no sensor is required */

    portENTER_CRITICAL(&g_node.state_lock);
    uint32_t alarm_seen = g_node.alarm_loops, fusion_seen = g_node.fusion_loops;
    portEXIT_CRITICAL(&g_node.state_lock);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        fx_ota_health_t h;
        portENTER_CRITICAL(&g_node.state_lock);
        const uint32_t alarm_now = g_node.alarm_loops, fusion_now = g_node.fusion_loops;
        const bool have_fusion = g_node.have_fusion;
        portEXIT_CRITICAL(&g_node.state_lock);
        h.ledger_ok = g_node.ledger_ok && g_node.ledger.appended > 0;
        h.alarm_alive = alarm_now != alarm_seen;
        h.fusion_alive = have_fusion && fusion_now != fusion_seen;
        alarm_seen = alarm_now;
        fusion_seen = fusion_now;
        h.sensors_ready = sensors_ready();
        h.sensors_expected = expected;
        h.gateway_synced = node_net_synced();

        const char *missing = NULL;
        const fx_ota_verdict_t verdict = fx_ota_confirm_step(&h, node_now_ms(), FX_OTA_CONFIRM_MS, &missing);
        if (verdict == FX_OTA_WAIT) {
            set_status("%s confirming: %s", app->version, missing);
            continue;
        }
        if (verdict == FX_OTA_CONFIRM) {
            if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
                ESP_LOGE(TAG, "the bootloader did not take the confirmation; the next restart may roll back");
            }
            portENTER_CRITICAL(&s_lock);
            s_pending_verify = false;
            portEXIT_CRITICAL(&s_lock);
            nvs_put_byte("sensors", h.sensors_ready);
            nvs_put_text("why", NULL);
            log_update(FX_OTA_CONFIRMED, app->version, from, have_image ? image : NULL, NULL);
            set_status("%s confirmed itself", app->version);
            ESP_LOGI(TAG, "%s confirmed", app->version);
            return;
        }
        char reason[112];
        snprintf(reason, sizeof(reason), "not confirmed within %u s: %s", (unsigned)(FX_OTA_CONFIRM_MS / 1000u),
                 missing);
        nvs_put_text("why", reason);
        log_update(FX_OTA_FAILED, app->version, from, have_image ? image : NULL, reason);
        ESP_LOGE(TAG, "%s; rolling back", reason);
        wait_for_ledger();
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return; /* not reached when the rollback is possible */
    }
}

/* The old image again, after a new one gave up or crashed before confirming: once per failed image. */
static void report_rollback(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    uint8_t part_digest[32];
    if (!bad || esp_ota_get_partition_description(bad, &desc) != ESP_OK ||
        esp_partition_get_sha256(bad, part_digest) != ESP_OK) {
        return;
    }
    char marker[65], reported[65];
    fx_hex_encode(part_digest, 32, marker);
    if (nvs_get_text("rb_done", reported, sizeof(reported)) && strcmp(reported, marker) == 0) {
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char version[FX_OTA_VERSION_MAX], from[FX_OTA_VERSION_MAX], why[112];
    memcpy(version, desc.version, sizeof(version));
    version[sizeof(version) - 1] = '\0';
    uint8_t image[32];
    const bool have_image = installing(version, from, sizeof(from), image);
    if (!have_image || !nvs_get_text("why", why, sizeof(why))) {
        snprintf(why, sizeof(why), "the new image did not confirm itself; the bootloader went back");
    }
    log_update(FX_OTA_ROLLED_BACK, version, app->version, have_image ? image : NULL, why);
    nvs_put_text("rb_done", marker);
    set_status("%s rolled back: %s", version, why);
    ESP_LOGW(TAG, "%s was rolled back: %s", version, why);
}

static void ota_task(void *arg)
{
    (void)arg;
    if (s_pending_verify) {
        confirm_or_roll_back();
    } else {
        report_rollback();
        nvs_put_byte("sensors", sensors_ready()); /* what the next image must match */
    }
    offer_t o;
    for (;;) {
        if (xQueueReceive(s_offers, &o, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        run_update(&o);
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        s_received = 0;
        portEXIT_CRITICAL(&s_lock);
    }
}

void node_ota_start(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    s_pending_verify = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                       state == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_pending_verify) {
        ESP_LOGW(TAG, "%s is a new image: it must confirm itself within %u s", esp_app_get_description()->version,
                 (unsigned)(FX_OTA_CONFIRM_MS / 1000u));
    }
    s_offers = xQueueCreate(1, sizeof(offer_t));
    if (!s_offers || xTaskCreate(ota_task, "ota", 8192, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "the OTA task did not start; updates are refused and a new image cannot confirm");
    }
}

void node_ota_describe(char *partition, size_t cap, bool *pending_verify)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (partition && cap) {
        snprintf(partition, cap, "%s", running ? running->label : "?");
    }
    if (pending_verify) {
        portENTER_CRITICAL(&s_lock);
        *pending_verify = s_pending_verify;
        portEXIT_CRITICAL(&s_lock);
    }
}

void node_ota_print_status(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    char status[sizeof(s_status)];
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_busy, pending = s_pending_verify;
    const uint32_t received = s_received;
    memcpy(status, s_status, sizeof(status));
    portEXIT_CRITICAL(&s_lock);

    printf("running  %s from %s%s\n", app->version, running ? running->label : "?",
           pending ? ", not confirmed yet: it rolls back unless it confirms within 60 s of boot" : "");
    printf("next     %s\n", next ? next->label : "none: this partition table has no second slot");
    if (busy) {
        printf("update   in progress, %" PRIu32 " bytes received\n", received);
    }
    printf("last     %s\n", status);
}
