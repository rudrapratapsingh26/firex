/*
 * Ledger hardware on the node: the device key and the LittleFS segment store.
 *
 * Device key. Generated on first boot from the hardware RNG (with the SAR ADC
 * entropy source enabled, so before any ADC driver starts) and kept in NVS.
 * Signatures are deterministic ECDSA (RFC 6979): no per-signature nonce to get
 * wrong. On the dev kit the private key sits in plain NVS; anyone with the
 * board and a USB cable can read it. That is the accepted M2-M5 risk the
 * threat model states, closed by the ATECC608B on the M6 PCB.
 *
 * Store. The ring buffer's segments are files in a LittleFS partition. Callers
 * serialise access (the node holds one ledger mutex around every call).
 */
#ifndef FX_LEDGER_HW_H
#define FX_LEDGER_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fx_ledger.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "sdkconfig.h"
#if CONFIG_FIREX_KEY_ATECC608B
#include "fx_secure.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* EVT board (CONFIG_FIREX_KEY_ATECC608B). The key is generated inside the ATECC608B's slot
 * 0 at the factory and never leaves it (docs/adr/0008-secure-element.md). Signing goes to the
 * chip, which makes its own nonce, so signatures are not RFC 6979; verification stays in
 * software against the public key. An unprovisioned chip opens no key, so the ledger stays
 * closed and the unit shows FAULT. */
typedef struct {
#if CONFIG_FIREX_KEY_ATECC608B
    fx_se_t se;
#else
    mbedtls_mpi d;
#endif
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    SemaphoreHandle_t lock;
    uint8_t pub[65];
    uint8_t device_id[16];
    bool created; /* generated on this boot */
    bool ready;
} fx_device_key_t;

/* Load the key from NVS, or generate and store one if none exists. A stored key
 * that fails validation is an error, never silently replaced. */
esp_err_t fx_device_key_open(fx_device_key_t *k);
fx_signer_t fx_device_key_signer(fx_device_key_t *k);
size_t fx_device_key_pem(const fx_device_key_t *k, char *out, size_t cap);

/* Mount a LittleFS partition. A blank partition is formatted; a partition that
 * holds data but will not mount is left alone and reported, because formatting
 * it would destroy evidence. */
esp_err_t fx_fs_mount(const char *label, const char *base_path, bool *formatted);
esp_err_t fx_fs_usage(const char *label, size_t *total_bytes, size_t *used_bytes);

typedef struct {
    char base[24];
    FILE *reader;
    uint32_t reader_seg;
} fx_fs_store_t;

void fx_fs_store_init(fx_fs_store_t *s, const char *base_path, fx_store_ops_t *ops);
/* Delete every segment file. Returns the number deleted, or -1. */
int fx_fs_store_wipe(fx_fs_store_t *s);

#ifdef __cplusplus
}
#endif
#endif /* FX_LEDGER_HW_H */
