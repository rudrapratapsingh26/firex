#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "fx_entry.h"
#include "fx_net_hw.h"
#include "fx_sha256.h"

#define REPLY_MAX 8192
#define PEM_MAX 3900u /* an NVS string holds at most 4000 bytes */

static const char *TAG = "provision";

const char *fx_prov_outcome_text(fx_prov_outcome_t o)
{
    switch (o) {
    case FX_PROV_OK: return "provisioned";
    case FX_PROV_NETWORK: return "the gateway did not answer";
    case FX_PROV_REJECTED: return "the gateway could not read the request";
    case FX_PROV_NO_PAIRING: return "no open pairing matches this unit's code: add the unit on the dashboard first";
    case FX_PROV_CONFLICT: return "the gateway knows this unit under a different ledger key";
    case FX_PROV_RATE_LIMITED: return "too many failed attempts at the gateway: wait ten minutes";
    case FX_PROV_BAD_REPLY: return "the answer was not a provisioning reply";
    case FX_PROV_BAD_MAC: return "the reply was not made with this unit's pairing code, so it was ignored";
    default: return "internal error";
    }
}

static char *dup_string(const cJSON *root, const char *key, size_t max)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) > max) {
        return NULL;
    }
    return strdup(v->valuestring);
}

static fx_prov_outcome_t parse_reply(const fx_prov_client_t *c, const char *nonce, const char *text, fx_net_creds_t *out)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        return FX_PROV_BAD_REPLY;
    }
    fx_prov_outcome_t result = FX_PROV_BAD_REPLY;
    const cJSON *site = cJSON_GetObjectItemCaseSensitive(root, "site_id");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "mqtt_host");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "mqtt_port");
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "mac");
    char device_hex[33];
    fx_hex_encode(c->device_id, 16, device_hex);

    if (cJSON_IsString(site) && strlen(site->valuestring) < sizeof(out->site_id) && cJSON_IsString(device) &&
        strcmp(device->valuestring, device_hex) == 0 && cJSON_IsString(host) &&
        strlen(host->valuestring) < sizeof(out->mqtt_host) && cJSON_IsNumber(port) && port->valuedouble >= 1.0 &&
        port->valuedouble <= 65535.0 && cJSON_IsString(mac)) {
        out->ca_pem = dup_string(root, "ca_pem", PEM_MAX);
        out->cert_pem = dup_string(root, "cert_pem", PEM_MAX);
        if (out->ca_pem && out->cert_pem) {
            const fx_prov_reply_t reply = {
                .site_id = site->valuestring,
                .mqtt_host = host->valuestring,
                .mqtt_port = (uint32_t)port->valueint,
                .ca_pem = out->ca_pem,
                .cert_pem = out->cert_pem,
            };
            if (fx_prov_reply_mac_ok(c->code, nonce, &reply, mac->valuestring)) {
                snprintf(out->site_id, sizeof(out->site_id), "%s", site->valuestring);
                snprintf(out->mqtt_host, sizeof(out->mqtt_host), "%s", host->valuestring);
                out->mqtt_port = (uint16_t)port->valueint;
                result = FX_PROV_OK;
            } else {
                result = FX_PROV_BAD_MAC;
            }
        }
    }
    cJSON_Delete(root);
    if (result != FX_PROV_OK) {
        fx_net_creds_free(out);
    }
    return result;
}

fx_prov_outcome_t fx_prov_run(const fx_prov_client_t *c, fx_net_creds_t *out, int *http_status)
{
    memset(out, 0, sizeof(*out));
    if (http_status) {
        *http_status = 0;
    }
    if (!c || !c->host || !c->code || !c->tls || !c->ledger_signer.sign_digest) {
        return FX_PROV_INTERNAL;
    }

    fx_prov_ids_t ids;
    fx_hex_encode(c->device_id, 16, ids.device_id);
    fx_hex_encode(c->ledger_pub, 65, ids.ledger_pub);
    fx_hex_encode(c->tls->pub, 65, ids.tls_pub);
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    fx_hex_encode(nonce, sizeof(nonce), ids.nonce);

    /* Both keys sign the message, so the gateway knows the unit holds them; the proof shows it holds the code. */
    char message[400];
    const size_t message_len = fx_prov_message(&ids, message, sizeof(message));
    uint8_t digest[32], sig[64];
    char ledger_sig[129], tls_sig[129], proof[65];
    if (message_len == 0) {
        return FX_PROV_INTERNAL;
    }
    fx_sha256(message, message_len, digest);
    if (c->ledger_signer.sign_digest(c->ledger_signer.ctx, digest, sig) != 0) {
        return FX_PROV_INTERNAL;
    }
    fx_hex_encode(sig, 64, ledger_sig);
    if (fx_tls_key_sign(c->tls, digest, sig) != 0) {
        return FX_PROV_INTERNAL;
    }
    fx_hex_encode(sig, 64, tls_sig);
    fx_prov_proof(c->code, &ids, proof);

    char body[1024];
    const size_t body_len = fx_prov_request_json(&ids, c->fw_version, ledger_sig, tls_sig, proof, body, sizeof(body));
    if (body_len == 0) {
        return FX_PROV_INTERNAL;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/api/provision", c->host, (unsigned)c->port);
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char *reply = malloc(REPLY_MAX);
    if (!client || !reply) {
        if (client) {
            esp_http_client_cleanup(client);
        }
        free(reply);
        return FX_PROV_INTERNAL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    fx_prov_outcome_t outcome = FX_PROV_NETWORK;
    if (esp_http_client_open(client, (int)body_len) == ESP_OK &&
        esp_http_client_write(client, body, (int)body_len) == (int)body_len && esp_http_client_fetch_headers(client) >= 0) {
        const int status = esp_http_client_get_status_code(client);
        if (http_status) {
            *http_status = status;
        }
        int got = esp_http_client_read_response(client, reply, REPLY_MAX - 1);
        reply[got > 0 ? got : 0] = '\0';
        switch (status) {
        case 200: outcome = parse_reply(c, ids.nonce, reply, out); break;
        case 400: outcome = FX_PROV_REJECTED; break;
        case 403: outcome = FX_PROV_NO_PAIRING; break;
        case 409: outcome = FX_PROV_CONFLICT; break;
        case 429: outcome = FX_PROV_RATE_LIMITED; break;
        default: outcome = FX_PROV_BAD_REPLY; break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "gateway answered %d: %.160s", status, reply);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(reply);
    return outcome;
}
