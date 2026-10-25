#include "fx_secure.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_FIREX_KEY_ATECC608B

#include "cryptoauthlib.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef CONFIG_ATCA_I2C_ADDRESS
#define CONFIG_ATCA_I2C_ADDRESS 0xC0
#endif

#define MAX_STEPS 8

static const char *TAG = "atecc";

static SemaphoreHandle_t s_lock;
static bool s_up;
static ATCAIfaceCfg s_cfg;

static bool take(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void give(void)
{
    xSemaphoreGive(s_lock);
}

/* Caller holds the lock. */
static bool session_up(void)
{
    if (s_up) {
        return true;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.iface_type = ATCA_I2C_IFACE;
    s_cfg.devtype = ATECC608;
    ATCA_IFACECFG_VALUE(&s_cfg, atcai2c.address) = (uint8_t)CONFIG_ATCA_I2C_ADDRESS;
    ATCA_IFACECFG_VALUE(&s_cfg, atcai2c.bus) = (uint8_t)CONFIG_FIREX_ATECC_I2C_PORT;
    ATCA_IFACECFG_VALUE(&s_cfg, atcai2c.baud) = 100000u;
    s_cfg.wake_delay = 1500;
    s_cfg.rx_retries = 20;
    const ATCA_STATUS st = atcab_init(&s_cfg);
    if (st != ATCA_SUCCESS) {
        ESP_LOGE(TAG, "atcab_init on I2C%d address 0x%02X: status 0x%02X", CONFIG_FIREX_ATECC_I2C_PORT,
                 CONFIG_ATCA_I2C_ADDRESS, (unsigned)st);
        return false;
    }
    s_up = true;
    return true;
}

/* Caller holds the lock. */
static bool read_config(fx_se_t *se)
{
    return atcab_read_config_zone(se->config) == ATCA_SUCCESS;
}

esp_err_t fx_se_open(fx_se_t *se)
{
    if (!se) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    if (!session_up()) {
        err = ESP_ERR_NOT_FOUND;
    } else if (atcab_read_serial_number(se->serial) != ATCA_SUCCESS || atcab_info(se->revision) != ATCA_SUCCESS ||
               !read_config(se)) {
        ESP_LOGE(TAG, "the secure element did not answer its serial number, revision and configuration");
        err = ESP_ERR_NOT_FOUND;
    }
    se->open = err == ESP_OK;
    give();
    return err;
}

fx_se_step_t fx_se_state(fx_se_t *se, const char **why)
{
    if (!se || !se->open) {
        if (why) {
            *why = "the secure element is not open";
        }
        return FX_SE_STEP_REFUSE;
    }
    if (!take()) {
        if (why) {
            *why = "no memory for the secure element lock";
        }
        return FX_SE_STEP_REFUSE;
    }
    const bool ok = read_config(se);
    give();
    if (!ok) {
        if (why) {
            *why = "the configuration zone could not be read";
        }
        return FX_SE_STEP_REFUSE;
    }
    return fx_se_next_step(se->config, why);
}

static void to_point(const uint8_t xy[64], uint8_t pub[65])
{
    pub[0] = 0x04;
    memcpy(pub + 1, xy, 64);
}

/* Caller holds the lock. */
static const char *run_step(fx_se_t *se, fx_se_step_t step, fx_se_progress_fn progress, void *ctx)
{
    switch (step) {
    case FX_SE_STEP_WRITE_CONFIG: {
        uint8_t want[FX_SE_CONFIG_SIZE];
        memcpy(want, se->config, sizeof(want));
        fx_se_config_apply(want);
        /* Only the two words FireX owns: SlotConfig[0..1] and KeyConfig[0..1]. */
        if (atcab_write_bytes_zone(ATCA_ZONE_CONFIG, 0, FX_SE_SLOT_CONFIG_AT(0), &want[FX_SE_SLOT_CONFIG_AT(0)], 4) !=
                ATCA_SUCCESS ||
            atcab_write_bytes_zone(ATCA_ZONE_CONFIG, 0, FX_SE_KEY_CONFIG_AT(0), &want[FX_SE_KEY_CONFIG_AT(0)], 4) !=
                ATCA_SUCCESS) {
            return "write_config: the write was refused";
        }
        if (!read_config(se) || memcmp(se->config, want, sizeof(want)) != 0) {
            return "write_config: the configuration read back differs from what was written";
        }
        break;
    }
    case FX_SE_STEP_LOCK_CONFIG:
        /* The chip locks only if its own CRC of the zone matches ours. */
        if (atcab_lock_config_zone_crc(fx_se_crc16(se->config, FX_SE_CONFIG_SIZE)) != ATCA_SUCCESS) {
            return "lock_config: the chip refused the lock (its configuration differs from the one read)";
        }
        break;
    case FX_SE_STEP_LOCK_DATA:
        if (atcab_lock_data_zone() != ATCA_SUCCESS) {
            return "lock_data: the chip refused the lock";
        }
        break;
    case FX_SE_STEP_GENKEY_LEDGER: {
        uint8_t xy[64];
        if (atcab_genkey(FX_SE_SLOT_LEDGER, xy) != ATCA_SUCCESS) {
            return "genkey_ledger: GenKey failed";
        }
        if (progress) {
            progress(FX_SE_STEP_GENKEY_LEDGER, ctx);
        }
        if (atcab_lock_data_slot(FX_SE_SLOT_LEDGER) != ATCA_SUCCESS) {
            return "lock_ledger_slot: the chip refused the slot lock";
        }
        step = FX_SE_STEP_LOCK_LEDGER_SLOT;
        break;
    }
    default:
        return "no step to run";
    }
    if (progress) {
        progress(step, ctx);
    }
    return read_config(se) ? NULL : "the configuration zone could not be read after a step";
}

esp_err_t fx_se_provision(fx_se_t *se, fx_se_progress_fn progress, void *ctx, uint8_t pub[65], const char **why)
{
    const char *reason = NULL;
    esp_err_t err = ESP_FAIL;
    if (!se || !se->open || !pub) {
        reason = "the secure element is not open";
    } else if (!take()) {
        reason = "no memory for the secure element lock";
    } else {
        if (!read_config(se)) {
            reason = "the configuration zone could not be read";
        }
        for (int i = 0; !reason && i < MAX_STEPS; i++) {
            const fx_se_step_t step = fx_se_next_step(se->config, &reason);
            if (step == FX_SE_STEP_REFUSE) {
                reason = reason ? reason : "refused";
                break;
            }
            if (step == FX_SE_STEP_DONE) {
                uint8_t xy[64];
                if (atcab_get_pubkey(FX_SE_SLOT_LEDGER, xy) != ATCA_SUCCESS) {
                    reason = "done, but the ledger public key could not be read";
                } else {
                    to_point(xy, pub);
                    err = ESP_OK;
                }
                break;
            }
            ESP_LOGW(TAG, "provisioning step %s", fx_se_step_name(step));
            reason = run_step(se, step, progress, ctx);
        }
        if (!reason && err != ESP_OK) {
            reason = "provisioning did not finish";
        }
        give();
    }
    if (why) {
        *why = err == ESP_OK ? NULL : reason;
    }
    return err;
}

esp_err_t fx_se_public_key(fx_se_t *se, uint16_t slot, uint8_t pub[65])
{
    if (!se || !se->open || !pub) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!take()) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t xy[64];
    const ATCA_STATUS st = atcab_get_pubkey(slot, xy);
    give();
    if (st != ATCA_SUCCESS) {
        return ESP_FAIL;
    }
    to_point(xy, pub);
    return ESP_OK;
}

int fx_se_sign(fx_se_t *se, uint16_t slot, const uint8_t digest[32], uint8_t sig[64])
{
    if (!se || !se->open || !digest || !sig) {
        return -1;
    }
    if (!take()) {
        return -1;
    }
    const ATCA_STATUS st = atcab_sign(slot, digest, sig);
    give();
    return st == ATCA_SUCCESS ? 0 : -1;
}

#else /* CONFIG_FIREX_KEY_ATECC608B */

esp_err_t fx_se_open(fx_se_t *se)
{
    if (se) {
        memset(se, 0, sizeof(*se));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

fx_se_step_t fx_se_state(fx_se_t *se, const char **why)
{
    (void)se;
    if (why) {
        *why = "this image has no secure element";
    }
    return FX_SE_STEP_REFUSE;
}

esp_err_t fx_se_provision(fx_se_t *se, fx_se_progress_fn progress, void *ctx, uint8_t pub[65], const char **why)
{
    (void)se;
    (void)progress;
    (void)ctx;
    (void)pub;
    if (why) {
        *why = "this image has no secure element";
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fx_se_public_key(fx_se_t *se, uint16_t slot, uint8_t pub[65])
{
    (void)se;
    (void)slot;
    (void)pub;
    return ESP_ERR_NOT_SUPPORTED;
}

int fx_se_sign(fx_se_t *se, uint16_t slot, const uint8_t digest[32], uint8_t sig[64])
{
    (void)se;
    (void)slot;
    (void)digest;
    (void)sig;
    return -1;
}

#endif /* CONFIG_FIREX_KEY_ATECC608B */
