#include "fx_prov.h"

#include <stdio.h>
#include <string.h>

#include "fx_entry.h"
#include "fx_sha256.h"

static const char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static const char CONTEXT[] = "firex-provision-v1";

void fx_pairing_code_from_random(const uint8_t random[10], char code[11])
{
    for (size_t i = 0; i < FX_PAIRING_CODE_LEN; i++) {
        code[i] = ALPHABET[random[i] & 31u];
    }
    code[FX_PAIRING_CODE_LEN] = '\0';
}

bool fx_pairing_code_normalise(const char *input, char code[11])
{
    size_t n = 0;
    for (const char *p = input; p && *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ' || c == '\t') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c == 'O') {
            c = '0';
        } else if (c == 'I' || c == 'L') {
            c = '1';
        }
        if (!strchr(ALPHABET, c) || c == '\0' || n >= FX_PAIRING_CODE_LEN) {
            return false;
        }
        code[n++] = c;
    }
    code[n] = '\0';
    return n == FX_PAIRING_CODE_LEN;
}

void fx_pairing_code_format(const char code[11], char out[12])
{
    memcpy(out, code, 5);
    out[5] = '-';
    memcpy(out + 6, code + 5, 5);
    out[11] = '\0';
}

size_t fx_prov_message(const fx_prov_ids_t *ids, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "%s\n%s\n%s\n%s\n%s", CONTEXT, ids->device_id, ids->ledger_pub, ids->tls_pub, ids->nonce);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

void fx_prov_proof(const char *code, const fx_prov_ids_t *ids, char proof_hex[65])
{
    char message[400];
    uint8_t mac[32];
    const size_t n = fx_prov_message(ids, message, sizeof(message));
    fx_hmac_sha256((const uint8_t *)code, strlen(code), (const uint8_t *)message, n, mac);
    fx_hex_encode(mac, 32, proof_hex);
}

bool fx_prov_reply_mac_ok(const char *code, const char *nonce, const fx_prov_reply_t *reply, const char *mac_hex)
{
    if (!code || !nonce || !reply || !mac_hex || !reply->site_id || !reply->mqtt_host || !reply->ca_pem || !reply->cert_pem ||
        strlen(mac_hex) != 64u) {
        return false;
    }
    uint8_t digest[32];
    char ca_hex[65], cert_hex[65];
    fx_sha256(reply->ca_pem, strlen(reply->ca_pem), digest);
    fx_hex_encode(digest, 32, ca_hex);
    fx_sha256(reply->cert_pem, strlen(reply->cert_pem), digest);
    fx_hex_encode(digest, 32, cert_hex);

    char text[512];
    const int n = snprintf(text, sizeof(text), "%s-reply\n%s\n%s\n%s\n%lu\n%s\n%s", CONTEXT, nonce, reply->site_id,
                           reply->mqtt_host, (unsigned long)reply->mqtt_port, ca_hex, cert_hex);
    if (n < 0 || (size_t)n >= sizeof(text)) {
        return false;
    }
    uint8_t mac[32];
    fx_hmac_sha256((const uint8_t *)code, strlen(code), (const uint8_t *)text, (size_t)n, mac);
    char expected[65];
    fx_hex_encode(mac, 32, expected);

    uint8_t diff = 0;
    for (size_t i = 0; i < 64u; i++) {
        char a = mac_hex[i];
        if (a >= 'A' && a <= 'F') {
            a = (char)(a - 'A' + 'a');
        }
        diff |= (uint8_t)(a ^ expected[i]);
    }
    return diff == 0;
}

size_t fx_prov_request_json(const fx_prov_ids_t *ids, const char *fw_version, const char *ledger_sig_hex,
                            const char *tls_sig_hex, const char *proof_hex, char *out, size_t cap)
{
    for (const char *p = fw_version; *p; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20u) {
            return 0; /* a version string that would need escaping is not a version string */
        }
    }
    const int n = snprintf(out, cap,
                           "{\"version\":1,\"device_id\":\"%s\",\"ledger_pub\":\"%s\",\"tls_pub\":\"%s\",\"fw_version\":\"%s\","
                           "\"nonce\":\"%s\",\"ledger_sig\":\"%s\",\"tls_sig\":\"%s\",\"proof\":\"%s\"}",
                           ids->device_id, ids->ledger_pub, ids->tls_pub, fw_version, ids->nonce, ledger_sig_hex, tls_sig_hex, proof_hex);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
