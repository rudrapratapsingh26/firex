#include "fx_ota.h"

#include <stdio.h>
#include <string.h>

#include "fx_cbor.h"
#include "fx_entry.h"

/* --- versions ---------------------------------------------------------------- */

#define VERSION_PARTS 4

typedef struct {
    unsigned long n[VERSION_PARTS];
    const char *pre; /* after '-', up to '+' or the end; NULL for a release */
    size_t pre_len;
} version_t;

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static void parse_version(const char *s, version_t *v)
{
    memset(v, 0, sizeof(*v));
    if (!s) {
        return;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (int i = 0; i < VERSION_PARTS && is_digit(*s); i++) {
        unsigned long x = 0;
        while (is_digit(*s)) {
            if (x < 100000000ul) { /* no real version gets here; it only keeps the arithmetic defined */
                x = x * 10ul + (unsigned long)(*s - '0');
            }
            s++;
        }
        v->n[i] = x;
        if (*s != '.') {
            break;
        }
        s++;
    }
    if (*s == '-') {
        s++;
        const char *plus = strchr(s, '+');
        v->pre = s;
        v->pre_len = plus ? (size_t)(plus - s) : strlen(s);
    }
}

int fx_ota_version_compare(const char *a, const char *b)
{
    version_t va, vb;
    parse_version(a, &va);
    parse_version(b, &vb);
    for (int i = 0; i < VERSION_PARTS; i++) {
        if (va.n[i] != vb.n[i]) {
            return va.n[i] < vb.n[i] ? -1 : 1;
        }
    }
    if (!va.pre || !vb.pre) {
        return va.pre ? -1 : vb.pre ? 1 : 0; /* a release sorts after its prereleases */
    }
    const size_t n = va.pre_len < vb.pre_len ? va.pre_len : vb.pre_len;
    const int c = memcmp(va.pre, vb.pre, n);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return va.pre_len == vb.pre_len ? 0 : (va.pre_len < vb.pre_len ? -1 : 1);
}

bool fx_ota_parse_sha256(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64u) {
        return false;
    }
    for (size_t i = 0; i < 32u; i++) {
        uint8_t byte = 0;
        for (size_t k = 0; k < 2u; k++) {
            const char c = hex[2u * i + k];
            uint8_t nibble;
            if (c >= '0' && c <= '9') {
                nibble = (uint8_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibble = (uint8_t)(c - 'a' + 10);
            } else {
                return false; /* the transport writes lowercase */
            }
            byte = (uint8_t)((byte << 4) | nibble);
        }
        out[i] = byte;
    }
    return true;
}

/* --- accepting a command --------------------------------------------------------- */

static bool valid_version(const char *v)
{
    if (!v || !v[0] || strlen(v) >= FX_OTA_VERSION_MAX) {
        return false;
    }
    for (const char *c = v; *c; c++) {
        if (*c < 0x21 || *c > 0x7E || *c == '"' || *c == '\\') {
            return false;
        }
    }
    return true;
}

const char *fx_ota_refusal(const fx_ota_node_t *node, const fx_ota_offer_t *offer, uint32_t partition_size)
{
    uint8_t digest[32];
    if (!node || !offer) {
        return "no command";
    }
    /* First whether the command makes sense, then whether this is the moment. */
    if (!valid_version(offer->version)) {
        return "the command's version is not valid";
    }
    if (!fx_ota_parse_sha256(offer->sha256, digest)) {
        return "the command's sha256 is not 64 lowercase hex digits";
    }
    if (!offer->url || strncmp(offer->url, "https://", 8) != 0 || strlen(offer->url) >= FX_OTA_URL_MAX) {
        return "the command's url is not an https URL";
    }
    if (offer->size == 0u) {
        return "the command gives no image size";
    }
    if (offer->size > partition_size) {
        return "the image is larger than the update partition";
    }
    if (node->update_in_progress) {
        return "an update is already in progress";
    }
    if (node->pending_verify) {
        return "the running image has not confirmed itself yet";
    }
    if (node->running_version && strcmp(offer->version, node->running_version) == 0) {
        return "that version is already running";
    }
    if (!node->have_fusion || node->fusion == FX_STATE_WARMUP) {
        return "the node is still warming up";
    }
    if (node->fusion != FX_STATE_NORMAL) {
        return "the node is not in NORMAL";
    }
    if (node->self_test_running) {
        return "a self-test is running";
    }
    if (node->injecting) {
        return "a flame injection is running";
    }
    return NULL;
}

/* --- rebooting when quiet --------------------------------------------------------- */

void fx_ota_quiet_init(fx_ota_quiet_t *q)
{
    q->counting = false;
    q->since_ms = 0;
}

bool fx_ota_quiet_step(fx_ota_quiet_t *q, const fx_ota_quiet_input_t *in, uint32_t quiet_ms)
{
    const bool quiet = in->have_fusion && in->fusion == FX_STATE_NORMAL && !in->alarm_active && !in->busy;
    if (!quiet) {
        q->counting = false; /* any break starts the minute again */
        return false;
    }
    if (!q->counting) {
        q->counting = true;
        q->since_ms = in->now_ms;
    }
    return in->now_ms - q->since_ms >= quiet_ms;
}

/* --- confirming a new image --------------------------------------------------------- */

fx_ota_verdict_t fx_ota_confirm_step(const fx_ota_health_t *h, uint32_t uptime_ms, uint32_t deadline_ms,
                                     const char **missing)
{
    const char *why = NULL;
    if (!h->ledger_ok) {
        why = "the ledger has not stored this boot's entry";
    } else if (!h->alarm_alive) {
        why = "the alarm task is not running";
    } else if (!h->fusion_alive) {
        why = "the fusion task is not running";
    } else if ((h->sensors_ready & h->sensors_expected) != h->sensors_expected) {
        why = "a sensor that was ready on the previous image is not ready";
    } else if (!h->gateway_synced) {
        why = "not connected and synced to the gateway";
    }
    if (missing) {
        *missing = why;
    }
    if (!why) {
        return FX_OTA_CONFIRM;
    }
    return uptime_ms >= deadline_ms ? FX_OTA_ROLLBACK : FX_OTA_WAIT;
}

/* --- warm restarts ------------------------------------------------------------------ */

bool fx_ota_heaters_kept_power(const char *reset_reason)
{
    static const char *const warm[] = {"software", "panic", "watchdog", "external"};
    if (!reset_reason) {
        return false;
    }
    for (size_t i = 0; i < sizeof(warm) / sizeof(warm[0]); i++) {
        if (strcmp(reset_reason, warm[i]) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t fx_ota_warmup_ms(uint32_t warmup_s, uint32_t warm_s, bool heaters_kept_power)
{
    const uint32_t s = heaters_kept_power && warm_s < warmup_s ? warm_s : warmup_s;
    return s * 1000u;
}

/* --- the ledger entry ---------------------------------------------------------------- */

const char *fx_ota_phase_name(fx_ota_phase_t phase)
{
    switch (phase) {
    case FX_OTA_ACCEPTED: return "accepted";
    case FX_OTA_DOWNLOADED: return "downloaded";
    case FX_OTA_CONFIRMED: return "confirmed";
    case FX_OTA_FAILED: return "failed";
    case FX_OTA_ROLLED_BACK: return "rolled_back";
    default: return "unknown";
    }
}

size_t fx_ev_fw_update(uint8_t *out, fx_ota_phase_t phase, const char *version, const char *from_version,
                       const uint8_t *image, const char *reason)
{
    static const uint8_t unknown[32] = {0};
    char why[97];
    snprintf(why, sizeof(why), "%.96s", reason ? reason : "");
    fx_map_t m;
    fx_map_init(&m);
    fx_map_text(&m, "phase", fx_ota_phase_name(phase));
    fx_map_text(&m, "version", version ? version : "");
    fx_map_text(&m, "from_version", from_version ? from_version : "");
    fx_map_bytes(&m, "image", image ? image : unknown, 32u);
    fx_map_text(&m, "reason", why);
    return fx_map_finish(&m, out, FX_PAYLOAD_MAX);
}
