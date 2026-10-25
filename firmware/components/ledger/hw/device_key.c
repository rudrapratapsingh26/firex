#include <string.h>

#include "bootloader_random.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "fx_entry.h"
#include "fx_ledger_hw.h"
#include "fx_sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "device_key";

/* Software verification against the public key, shared by both backends: the node checks
 * its own signatures (boot self-check, `ledger check sig`) without asking the chip. */
static bool verify_with_point(fx_device_key_t *k, const uint8_t digest[32], const uint8_t sig[64])
{
    if (!k || !k->lock) {
        return false;
    }
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = mbedtls_mpi_read_binary(&r, sig, 32) == 0 && mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0;
    if (ok) {
        xSemaphoreTake(k->lock, portMAX_DELAY);
        ok = mbedtls_ecdsa_verify(&k->grp, digest, 32, &k->Q, &r, &s) == 0;
        xSemaphoreGive(k->lock);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return ok;
}

static bool verify_digest(void *ctx, const uint8_t digest[32], const uint8_t sig[64])
{
    return verify_with_point((fx_device_key_t *)ctx, digest, sig);
}

static int sign_digest(void *ctx, const uint8_t digest[32], uint8_t sig[64]);

/* Shared by both backends: the key signs and verifies before it may start a chain. */
static esp_err_t self_check(fx_device_key_t *k)
{
    uint8_t digest[32], sig[64];
    fx_sha256("firex device key self-check", 27, digest);
    if (sign_digest(k, digest, sig) != 0 || !verify_digest(k, digest, sig)) {
        ESP_LOGE(TAG, "sign/verify self-check failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t begin(fx_device_key_t *k)
{
    memset(k, 0, sizeof(*k));
    mbedtls_ecp_group_init(&k->grp);
    mbedtls_ecp_point_init(&k->Q);
#if !CONFIG_FIREX_KEY_ATECC608B
    mbedtls_mpi_init(&k->d);
#endif
    k->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(k->lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_FALSE(mbedtls_ecp_group_load(&k->grp, MBEDTLS_ECP_DP_SECP256R1) == 0, ESP_FAIL, TAG, "P-256 group");
    return ESP_OK;
}

#if CONFIG_FIREX_KEY_ATECC608B

/* --- ATECC608B (EVT) ---------------------------------------------------------------------- */

static int sign_digest(void *ctx, const uint8_t digest[32], uint8_t sig[64])
{
    fx_device_key_t *k = (fx_device_key_t *)ctx;
    return k ? fx_se_sign(&k->se, FX_SE_SLOT_LEDGER, digest, sig) : -1;
}

esp_err_t fx_device_key_open(fx_device_key_t *k)
{
    ESP_RETURN_ON_ERROR(begin(k), TAG, "key context");

    esp_err_t err = fx_se_open(&k->se);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ATECC608B answered: the device key is unavailable and the ledger stays closed");
        return err;
    }
    const char *why = NULL;
    const fx_se_step_t step = fx_se_state(&k->se, &why);
    if (step != FX_SE_STEP_DONE) {
        ESP_LOGE(TAG, "the secure element is not provisioned (next step %s%s%s): see `se status`",
                 fx_se_step_name(step), why ? ": " : "", why ? why : "");
        return ESP_ERR_INVALID_STATE;
    }
    if (fx_se_public_key(&k->se, FX_SE_SLOT_LEDGER, k->pub) != ESP_OK) {
        ESP_LOGE(TAG, "the ledger public key could not be read from the secure element");
        return ESP_FAIL;
    }
    if (mbedtls_ecp_point_read_binary(&k->grp, &k->Q, k->pub, 65) != 0 || mbedtls_ecp_check_pubkey(&k->grp, &k->Q) != 0) {
        ESP_LOGE(TAG, "the secure element returned an invalid public key");
        return ESP_ERR_INVALID_STATE;
    }
    fx_device_id(k->pub, k->device_id);
    ESP_RETURN_ON_ERROR(self_check(k), TAG, "self-check");
    k->ready = true;
    return ESP_OK;
}

#else /* CONFIG_FIREX_KEY_ATECC608B */

/* --- software key in NVS (dev kit) ------------------------------------------------------ */

#define NS "fx_key"

static int hw_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static int sign_digest(void *ctx, const uint8_t digest[32], uint8_t sig[64])
{
    fx_device_key_t *k = (fx_device_key_t *)ctx;
    if (!k || !k->lock) {
        return -1;
    }
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    xSemaphoreTake(k->lock, portMAX_DELAY);
    int rc = mbedtls_ecdsa_sign_det_ext(&k->grp, &r, &s, &k->d, digest, 32, MBEDTLS_MD_SHA256, hw_rng, NULL);
    xSemaphoreGive(k->lock);
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&r, sig, 32);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return rc == 0 ? 0 : -1;
}

static esp_err_t store(const uint8_t priv[32], const uint8_t pub[65])
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(h, "priv", priv, 32);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "pub", pub, 65);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t fx_device_key_open(fx_device_key_t *k)
{
    ESP_RETURN_ON_ERROR(begin(k), TAG, "key context");

    uint8_t priv[32];
    size_t priv_len = sizeof(priv), pub_len = sizeof(k->pub);
    bool stored = false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        stored = nvs_get_blob(h, "priv", priv, &priv_len) == ESP_OK && priv_len == 32 &&
                 nvs_get_blob(h, "pub", k->pub, &pub_len) == ESP_OK && pub_len == 65;
        nvs_close(h);
    }

    esp_err_t result = ESP_OK;
    if (stored) {
        if (mbedtls_mpi_read_binary(&k->d, priv, 32) != 0 || mbedtls_ecp_check_privkey(&k->grp, &k->d) != 0 ||
            mbedtls_ecp_point_read_binary(&k->grp, &k->Q, k->pub, 65) != 0 ||
            mbedtls_ecp_check_pubkey(&k->grp, &k->Q) != 0) {
            ESP_LOGE(TAG, "the stored device key is invalid; refusing to replace it");
            result = ESP_ERR_INVALID_STATE;
        }
    } else {
        /* True randomness for the one value that must never repeat. The SAR ADC
         * entropy source is on only for the duration, before any ADC driver. */
        bootloader_random_enable();
        const int rc = mbedtls_ecp_gen_keypair(&k->grp, &k->d, &k->Q, hw_rng, NULL);
        bootloader_random_disable();
        size_t olen = 0;
        if (rc != 0 ||
            mbedtls_ecp_point_write_binary(&k->grp, &k->Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, k->pub,
                                           sizeof(k->pub)) != 0 ||
            olen != 65 || mbedtls_mpi_write_binary(&k->d, priv, 32) != 0) {
            ESP_LOGE(TAG, "key generation failed");
            result = ESP_FAIL;
        } else if (store(priv, k->pub) != ESP_OK) {
            ESP_LOGE(TAG, "could not store the new key in NVS");
            result = ESP_FAIL;
        } else {
            k->created = true;
        }
    }
    memset(priv, 0, sizeof(priv));
    if (result != ESP_OK) {
        return result;
    }

    fx_device_id(k->pub, k->device_id);
    ESP_RETURN_ON_ERROR(self_check(k), TAG, "self-check");
    k->ready = true;
    return ESP_OK;
}

#endif /* CONFIG_FIREX_KEY_ATECC608B */

fx_signer_t fx_device_key_signer(fx_device_key_t *k)
{
    fx_signer_t s = {sign_digest, verify_digest, k};
    return s;
}

size_t fx_device_key_pem(const fx_device_key_t *k, char *out, size_t cap)
{
    return (k && k->ready) ? fx_spki_pem(k->pub, out, cap) : 0;
}
