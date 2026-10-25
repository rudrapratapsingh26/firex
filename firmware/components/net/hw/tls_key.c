#include <stdlib.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "fx_net_hw.h"
#include "fx_sha256.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "nvs.h"

#define NS "fx_tls"
#define PEM_MAX 512u

static const char *TAG = "tls_key";

static int hw_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static bool load(const fx_tls_key_t *k, mbedtls_ecp_group *grp, mbedtls_mpi *d, mbedtls_ecp_point *Q)
{
    return mbedtls_ecp_group_load(grp, MBEDTLS_ECP_DP_SECP256R1) == 0 && mbedtls_mpi_read_binary(d, k->priv, 32) == 0 &&
           mbedtls_ecp_check_privkey(grp, d) == 0 && mbedtls_ecp_point_read_binary(grp, Q, k->pub, 65) == 0 &&
           mbedtls_ecp_check_pubkey(grp, Q) == 0;
}

int fx_tls_key_sign(const fx_tls_key_t *k, const uint8_t digest[32], uint8_t sig[64])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    mbedtls_ecp_point_init(&Q);
    int rc = (k && load(k, &grp, &d, &Q)) ? 0 : -1;
    if (rc == 0) {
        rc = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, digest, 32, MBEDTLS_MD_SHA256, hw_rng, NULL);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&r, sig, 32);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    }
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    return rc == 0 ? 0 : -1;
}

static bool verify(const fx_tls_key_t *k, const uint8_t digest[32], const uint8_t sig[64])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    mbedtls_ecp_point_init(&Q);
    bool ok = load(k, &grp, &d, &Q) && mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
              mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 && mbedtls_ecdsa_verify(&grp, digest, 32, &Q, &r, &s) == 0;
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    return ok;
}

static esp_err_t generate(fx_tls_key_t *k)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    /* The SAR ADC entropy source is on only for the duration, before any ADC driver. */
    bootloader_random_enable();
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) {
        rc = mbedtls_ecp_gen_keypair(&grp, &d, &Q, hw_rng, NULL);
    }
    bootloader_random_disable();

    size_t olen = 0;
    if (rc == 0) {
        rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, k->pub, sizeof(k->pub));
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&d, k->priv, sizeof(k->priv));
    }
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    if (rc != 0 || olen != 65u) {
        ESP_LOGE(TAG, "TLS key generation failed");
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "priv", k->priv, 32);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "pub", k->pub, 65);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not store the new TLS key: %s", esp_err_to_name(err));
        return err;
    }
    k->created = true;
    return ESP_OK;
}

esp_err_t fx_tls_key_open(fx_tls_key_t *k)
{
    memset(k, 0, sizeof(*k));
    size_t priv_len = sizeof(k->priv), pub_len = sizeof(k->pub);
    bool stored = false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        stored = nvs_get_blob(h, "priv", k->priv, &priv_len) == ESP_OK && priv_len == 32u &&
                 nvs_get_blob(h, "pub", k->pub, &pub_len) == ESP_OK && pub_len == 65u;
        nvs_close(h);
    }

    esp_err_t err = stored ? ESP_OK : generate(k);
    if (err != ESP_OK) {
        memset(k->priv, 0, sizeof(k->priv));
        return err;
    }

    /* A key that cannot sign and verify its own signature must not be offered to the gateway. */
    static const char CHECK[] = "firex tls key self-check";
    uint8_t digest[32], sig[64];
    fx_sha256(CHECK, sizeof(CHECK) - 1u, digest);
    if (fx_tls_key_sign(k, digest, sig) != 0 || !verify(k, digest, sig)) {
        if (stored) {
            ESP_LOGE(TAG, "the stored TLS key is invalid; refusing to replace it");
        } else {
            ESP_LOGE(TAG, "TLS key self-check failed");
        }
        memset(k->priv, 0, sizeof(k->priv));
        return stored ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    k->ready = true;
    return ESP_OK;
}

const char *fx_tls_key_pem(fx_tls_key_t *k)
{
    if (!k || !k->ready) {
        return NULL;
    }
    if (k->pem) {
        return k->pem;
    }
    char *pem = calloc(1, PEM_MAX);
    if (!pem) {
        return NULL;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc == 0) {
        rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), k->priv, sizeof(k->priv));
    }
    if (rc == 0) {
        rc = mbedtls_ecp_keypair_calc_public(mbedtls_pk_ec(pk), hw_rng, NULL);
    }
    if (rc == 0) {
        rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)pem, PEM_MAX);
    }
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not write the TLS key as PEM (-0x%04x)", (unsigned)-rc);
        free(pem);
        return NULL;
    }
    k->pem = pem;
    return pem;
}
