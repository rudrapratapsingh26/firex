/*
 * Pairing codes and the provisioning exchange (docs/transport.md section 1).
 * Pure C: the same strings the gateway builds in gateway/server/pairing.ts,
 * tested against its output in firmware/test/provision_golden.h.
 */
#ifndef FX_PROV_H
#define FX_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_PAIRING_CODE_LEN 10u

/* Ten Crockford base32 characters from the low five bits of ten random bytes. */
void fx_pairing_code_from_random(const uint8_t random[10], char code[11]);
/* Upper case, separators removed, O to 0, I and L to 1. False if not a valid code. */
bool fx_pairing_code_normalise(const char *input, char code[11]);
/* XXXXX-XXXXX, for the label and the console. */
void fx_pairing_code_format(const char code[11], char out[12]);

typedef struct {
    char device_id[33];   /* 32 hex */
    char ledger_pub[131]; /* 130 hex: 04 || X || Y */
    char tls_pub[131];
    char nonce[33];       /* 32 hex */
} fx_prov_ids_t;

/* "firex-provision-v1\n" device_id "\n" ledger_pub "\n" tls_pub "\n" nonce. Returns length or 0. */
size_t fx_prov_message(const fx_prov_ids_t *ids, char *out, size_t cap);

/* HMAC-SHA256(code, message), 64 lowercase hex. */
void fx_prov_proof(const char *code, const fx_prov_ids_t *ids, char proof_hex[65]);

typedef struct {
    const char *site_id;
    const char *mqtt_host;
    uint32_t mqtt_port;
    const char *ca_pem;
    const char *cert_pem;
} fx_prov_reply_t;

/* True when mac_hex is the MAC the gateway must have computed with the same code. Constant time. */
bool fx_prov_reply_mac_ok(const char *code, const char *nonce, const fx_prov_reply_t *reply, const char *mac_hex);

/* The request body. Returns length or 0 if it does not fit. */
size_t fx_prov_request_json(const fx_prov_ids_t *ids, const char *fw_version, const char *ledger_sig_hex,
                            const char *tls_sig_hex, const char *proof_hex, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* FX_PROV_H */
