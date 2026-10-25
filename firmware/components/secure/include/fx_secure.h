/*
 * The ATECC608B on the EVT board (M6), over cryptoauthlib.
 *
 * One chip, one cryptoauthlib session for the life of the boot: every atcab_* call in the
 * firmware goes through this module and its mutex. That is why esp-tls's own secure element
 * support stays off: it runs atcab_init for each connection and atcab_release when one
 * closes, which would tear the session down under the ledger task.
 *
 * The chip sits on its own I2C controller (CONFIG_FIREX_ATECC_I2C_PORT) and pins
 * (CONFIG_ATCA_I2C_SDA_PIN, CONFIG_ATCA_I2C_SCL_PIN): cryptoauthlib's ESP32 HAL creates that
 * bus itself. In an image built without CONFIG_FIREX_KEY_ATECC608B every call returns
 * ESP_ERR_NOT_SUPPORTED.
 */
#ifndef FX_SECURE_H
#define FX_SECURE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fx_se_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool open;
    uint8_t serial[9];
    uint8_t revision[4];
    uint8_t config[FX_SE_CONFIG_SIZE]; /* as last read off the chip */
} fx_se_t;

/* Start the session if it is not running, then read the serial number, revision and
 * configuration zone. ESP_ERR_NOT_FOUND if no chip answers. */
esp_err_t fx_se_open(fx_se_t *se);

/* Re-read the configuration zone and name the next provisioning step (fx_se_next_step). */
fx_se_step_t fx_se_state(fx_se_t *se, const char **why);

/* Called as each step completes during fx_se_provision. */
typedef void (*fx_se_progress_fn)(fx_se_step_t done, void *ctx);

/* Run every remaining provisioning step. Irreversible: configuration and data zone locks
 * are permanent. On ESP_OK `pub` holds the ledger key, 0x04 || X || Y. On failure `why`
 * names the step and the reason. */
esp_err_t fx_se_provision(fx_se_t *se, fx_se_progress_fn progress, void *ctx, uint8_t pub[65], const char **why);

/* The public key of a slot's private key, 0x04 || X || Y. */
esp_err_t fx_se_public_key(fx_se_t *se, uint16_t slot, uint8_t pub[65]);

/* ECDSA P-256 over a 32-byte digest with a slot's key, raw r || s. 0 on success. The nonce
 * comes from the chip's generator, so signatures are not RFC 6979 deterministic. */
int fx_se_sign(fx_se_t *se, uint16_t slot, const uint8_t digest[32], uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
#endif /* FX_SECURE_H */
