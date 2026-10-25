#include "fx_se_config.h"

static uint16_t get_u16(const uint8_t *cfg, unsigned at)
{
    return (uint16_t)(cfg[at] | (cfg[at + 1u] << 8));
}

static void put_u16(uint8_t *cfg, unsigned at, uint16_t v)
{
    cfg[at] = (uint8_t)(v & 0xFFu);
    cfg[at + 1u] = (uint8_t)(v >> 8);
}

uint16_t fx_se_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;
    for (size_t i = 0; data && i < len; i++) {
        for (uint8_t mask = 0x01u; mask != 0u; mask = (uint8_t)(mask << 1)) {
            const unsigned data_bit = (data[i] & mask) ? 1u : 0u;
            const unsigned crc_bit = (unsigned)(crc >> 15);
            crc = (uint16_t)(crc << 1);
            if (data_bit != crc_bit) {
                crc ^= 0x8005u;
            }
        }
    }
    return crc;
}

void fx_se_config_apply(uint8_t cfg[FX_SE_CONFIG_SIZE])
{
    if (!cfg) {
        return;
    }
    for (unsigned slot = FX_SE_SLOT_LEDGER; slot <= FX_SE_SLOT_TLS; slot++) {
        put_u16(cfg, FX_SE_SLOT_CONFIG_AT(slot), FX_SE_SLOT_CONFIG);
        put_u16(cfg, FX_SE_KEY_CONFIG_AT(slot), FX_SE_KEY_CONFIG);
    }
}

void fx_se_config_view(const uint8_t cfg[FX_SE_CONFIG_SIZE], fx_se_config_view_t *out)
{
    if (!out) {
        return;
    }
    *out = (fx_se_config_view_t){0};
    if (!cfg) {
        return;
    }
    const uint16_t slot_locked = get_u16(cfg, FX_SE_SLOT_LOCKED_AT);
    out->config_locked = cfg[FX_SE_LOCK_CONFIG_AT] != FX_SE_UNLOCKED;
    out->data_locked = cfg[FX_SE_LOCK_VALUE_AT] != FX_SE_UNLOCKED;
    out->ledger_slot_locked = (slot_locked & (1u << FX_SE_SLOT_LEDGER)) == 0u;
    out->tls_slot_locked = (slot_locked & (1u << FX_SE_SLOT_TLS)) == 0u;
    out->slots_as_required = true;
    for (unsigned slot = FX_SE_SLOT_LEDGER; slot <= FX_SE_SLOT_TLS; slot++) {
        if (get_u16(cfg, FX_SE_SLOT_CONFIG_AT(slot)) != FX_SE_SLOT_CONFIG ||
            get_u16(cfg, FX_SE_KEY_CONFIG_AT(slot)) != FX_SE_KEY_CONFIG) {
            out->slots_as_required = false;
        }
    }
}

fx_se_step_t fx_se_next_step(const uint8_t cfg[FX_SE_CONFIG_SIZE], const char **why)
{
    const char *reason = NULL;
    fx_se_step_t step = FX_SE_STEP_REFUSE;
    fx_se_config_view_t v;
    fx_se_config_view(cfg, &v);

    if (!cfg) {
        reason = "no configuration zone";
    } else if (!v.config_locked) {
        if (v.data_locked) {
            reason = "the data zone is locked but the configuration zone is not";
        } else if (v.ledger_slot_locked) {
            reason = "slot 0 is marked locked in an unlocked configuration: GenKey could never run";
        } else {
            step = v.slots_as_required ? FX_SE_STEP_LOCK_CONFIG : FX_SE_STEP_WRITE_CONFIG;
        }
    } else if (!v.slots_as_required) {
        reason = "the configuration zone is locked with a slot 0 or 1 configuration FireX did not write";
    } else if (!v.data_locked) {
        if (v.ledger_slot_locked) {
            reason = "slot 0 is locked before the data zone: it can never hold a key";
        } else {
            step = FX_SE_STEP_LOCK_DATA;
        }
    } else if (!v.ledger_slot_locked) {
        step = FX_SE_STEP_GENKEY_LEDGER;
    } else {
        step = FX_SE_STEP_DONE;
    }
    if (why) {
        *why = reason;
    }
    return step;
}

const char *fx_se_step_name(fx_se_step_t step)
{
    switch (step) {
    case FX_SE_STEP_WRITE_CONFIG: return "write_config";
    case FX_SE_STEP_LOCK_CONFIG: return "lock_config";
    case FX_SE_STEP_LOCK_DATA: return "lock_data";
    case FX_SE_STEP_GENKEY_LEDGER: return "genkey_ledger";
    case FX_SE_STEP_LOCK_LEDGER_SLOT: return "lock_ledger_slot";
    case FX_SE_STEP_DONE: return "done";
    case FX_SE_STEP_REFUSE:
    default: return "refuse";
    }
}
