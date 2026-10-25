/*
 * Over-the-air updates on the node (M5): the decisions, with nothing from
 * ESP-IDF in them, so they are host-tested (firmware/test/test_ota.c).
 * main/ota_task.c carries them out.
 *
 *   accept    an `ota` command only in NORMAL, outside warm-up, with no self-test or
 *             injection running, from a confirmed image, for a different version
 *   reboot    into the downloaded image only once fusion has been NORMAL for
 *             FX_OTA_QUIET_MS and the alarm is neither CRITICAL nor SILENCED
 *   confirm   a new image proves itself within FX_OTA_CONFIRM_MS of boot, or rolls back
 *   warm-up   after a reset that kept the MQ heaters powered, the shorter fx_warm_s
 *
 * Every outcome becomes a fw_update ledger entry (SPEC.md v0.5).
 */
#ifndef FX_OTA_H
#define FX_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_OTA_CONFIRM_MS 60000u
#define FX_OTA_QUIET_MS 60000u
#define FX_OTA_VERSION_MAX 32u /* esp_app_desc_t.version, with its NUL */
#define FX_OTA_URL_MAX 160u

/* --- versions ---------------------------------------------------------------- */

/* Semver-ish ordering: dot-separated numbers, then a release sorts after any
 * "-prerelease" of the same numbers; "+build" is ignored. <0, 0 or >0. Two
 * strings that order equal may still be different images: acceptance compares
 * the whole string. */
int fx_ota_version_compare(const char *a, const char *b);

/* 64 lowercase hex digits to 32 bytes. */
bool fx_ota_parse_sha256(const char *hex, uint8_t out[32]);

/* --- image kind (M6) ---------------------------------------------------------------- */

/* Every image from M6 carries this record in ESP-IDF's custom app description, at a
 * fixed offset right after esp_app_desc_t (main/image_kind.c). The dev kit, the EVT board
 * and the EVT production images are signed with the same release key, so without it a
 * production unit would install a signed development image and lose its privacy locks. */
#define FX_OTA_KIND_MAGIC "FXKIND1"
#define FX_OTA_KIND_MAX 24u
#define FX_OTA_KIND_OFFSET 288u /* image header 24 + segment header 8 + esp_app_desc_t 256 */

typedef struct {
    char magic[8];              /* FX_OTA_KIND_MAGIC with its NUL */
    char kind[FX_OTA_KIND_MAX]; /* "devkit", "evt" or "evt-production", NUL-terminated */
} fx_ota_image_kind_t;

/* NULL when an image carrying `candidate` may replace an image of kind `running`: the
 * same kind, exactly. Otherwise why not, in words for the ledger. */
const char *fx_ota_kind_refusal(const fx_ota_image_kind_t *candidate, const char *running);

/* --- accepting a command --------------------------------------------------------- */

typedef struct {
    bool have_fusion;
    fx_state_t fusion;
    bool self_test_running;
    bool injecting;
    bool update_in_progress;
    bool pending_verify; /* the running image has not confirmed itself yet */
    const char *running_version;
} fx_ota_node_t;

typedef struct {
    const char *version;
    uint32_t size;
    const char *sha256; /* hex */
    const char *url;
} fx_ota_offer_t;

/* NULL to accept, otherwise why not, in words for the ledger and the console. */
const char *fx_ota_refusal(const fx_ota_node_t *node, const fx_ota_offer_t *offer, uint32_t partition_size);

/* --- rebooting when quiet --------------------------------------------------------- */

typedef struct {
    bool counting;
    uint32_t since_ms;
} fx_ota_quiet_t;

typedef struct {
    bool have_fusion;
    fx_state_t fusion;
    bool alarm_active;  /* the alarm is CRITICAL or SILENCED */
    bool busy;          /* a self-test or an injection is running */
    uint32_t now_ms;
} fx_ota_quiet_input_t;

void fx_ota_quiet_init(fx_ota_quiet_t *q);
/* Feed the node's state; true once it has been quiet for quiet_ms without a break. */
bool fx_ota_quiet_step(fx_ota_quiet_t *q, const fx_ota_quiet_input_t *in, uint32_t quiet_ms);

/* --- confirming a new image --------------------------------------------------------- */

typedef struct {
    bool ledger_ok;          /* the ledger is open and this boot's entry is stored */
    bool alarm_alive;        /* the alarm task's loop count advanced */
    bool fusion_alive;       /* the fusion task's loop count advanced */
    uint8_t sensors_ready;   /* bit i: sensor i is ready */
    uint8_t sensors_expected;/* the sensors that were ready on the image before */
    bool gateway_synced;     /* connected to the gateway and synced, so it can be updated again */
} fx_ota_health_t;

typedef enum {
    FX_OTA_WAIT = 0,
    FX_OTA_CONFIRM,
    FX_OTA_ROLLBACK
} fx_ota_verdict_t;

/* CONFIRM as soon as every criterion holds; ROLLBACK once uptime reaches the
 * deadline without that; WAIT before. `missing` names the first criterion that
 * does not hold (NULL when all do). */
fx_ota_verdict_t fx_ota_confirm_step(const fx_ota_health_t *h, uint32_t uptime_ms, uint32_t deadline_ms,
                                     const char **missing);

/* --- warm restarts ------------------------------------------------------------------ */

/* The MQ heaters run from the 5 V supply, not from the MCU: a software, panic,
 * watchdog or external (EN) reset leaves them hot. Power-on, brownout and
 * anything unknown are cold. */
bool fx_ota_heaters_kept_power(const char *reset_reason);
uint32_t fx_ota_warmup_ms(uint32_t warmup_s, uint32_t warm_s, bool heaters_kept_power);

/* --- the ledger entry ---------------------------------------------------------------- */

typedef enum {
    FX_OTA_ACCEPTED = 0,
    FX_OTA_DOWNLOADED,
    FX_OTA_CONFIRMED,
    FX_OTA_FAILED,
    FX_OTA_ROLLED_BACK
} fx_ota_phase_t;

const char *fx_ota_phase_name(fx_ota_phase_t phase);

/* fw_update (SPEC.md v0.5). image may be NULL (unknown: 32 zero bytes), reason
 * NULL (""). Returns the length, or 0. */
size_t fx_ev_fw_update(uint8_t *out, fx_ota_phase_t phase, const char *version, const char *from_version,
                       const uint8_t *image, const char *reason);

#ifdef __cplusplus
}
#endif
#endif /* FX_OTA_H */
