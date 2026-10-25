/*
 * The node's network layer over ESP-IDF (M3): the TLS key, network settings in
 * NVS, Wi-Fi with SoftAP onboarding, and the provisioning client. The protocol
 * logic it drives is pure C and host-tested (fx_prov.h, fx_sync.h).
 *
 * TLS key. A second P-256 key, separate from the ledger key, so the MQTT
 * connection can be re-keyed without touching the signed record. Generated on
 * first boot like the device key and kept in NVS namespace fx_tls. The same
 * M2-M5 caveat applies: anyone with the board and a USB cable can read it,
 * until the secure element on the M6 PCB.
 *
 * Pairing code. Ten Crockford base32 characters from the hardware RNG on first
 * boot, kept in NVS and printed on the unit's label. It is the proof of
 * possession for Wi-Fi onboarding and the key of the gateway provisioning proof.
 */
#ifndef FX_NET_HW_H
#define FX_NET_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fx_ledger.h"
#include "fx_prov.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- TLS key ------------------------------------------------------------------ */

typedef struct {
    uint8_t priv[32];
    uint8_t pub[65];
    bool created; /* generated on this boot */
    bool ready;
    char *pem;    /* the private key as PEM for esp-mqtt, built on first use */
} fx_tls_key_t;

/* Load the key from NVS or generate one. Call before any ADC driver starts. A
 * stored key that fails validation is an error, never silently replaced. */
esp_err_t fx_tls_key_open(fx_tls_key_t *k);
/* ECDSA-P256 over a SHA-256 digest, raw r || s. 0 on success. */
int fx_tls_key_sign(const fx_tls_key_t *k, const uint8_t digest[32], uint8_t sig[64]);
/* The private key as PEM, or NULL. Owned by the key. */
const char *fx_tls_key_pem(fx_tls_key_t *k);

/* --- settings in NVS ---------------------------------------------------------- */

/* The unit's pairing code, normalised (10 characters). Generated and stored on
 * first boot; call before any ADC driver starts. */
esp_err_t fx_net_pairing_code(char code[11]);

#define FX_NET_HOST_MAX 64u

/* The gateway to provision with. ESP_ERR_NVS_NOT_FOUND until one is set. */
esp_err_t fx_net_gateway_get(char host[FX_NET_HOST_MAX], uint16_t *port);
esp_err_t fx_net_gateway_set(const char *host, uint16_t port);
/* host or host:port, from a person or the onboarding app. False unless the host
 * is a plain host name or IPv4 address and the port is 1-65535. */
bool fx_net_parse_host_port(const char *text, uint16_t default_port, char host[FX_NET_HOST_MAX], uint16_t *port);

typedef struct {
    char site_id[33];
    char mqtt_host[FX_NET_HOST_MAX];
    uint16_t mqtt_port;
    char *ca_pem;   /* heap */
    char *cert_pem; /* heap */
} fx_net_creds_t;

/* ESP_ERR_NVS_NOT_FOUND until the unit is provisioned. */
esp_err_t fx_net_creds_load(fx_net_creds_t *c);
esp_err_t fx_net_creds_store(const fx_net_creds_t *c);
void fx_net_creds_free(fx_net_creds_t *c);
/* Forget the gateway credentials, and the gateway address too when `gateway`
 * is set. The TLS key and the pairing code stay: they belong to the unit. */
esp_err_t fx_net_creds_erase(bool gateway);

/* --- Wi-Fi ---------------------------------------------------------------------- */

/* With a stored network the unit joins it. Without one it opens a SoftAP named
 * `service_name` for the ESP SoftAP Provisioning app: security 1 with `pop` as
 * the proof of possession, plus the "custom-data" endpoint, which takes the
 * gateway's host[:port]. */
esp_err_t fx_wifi_start(const char *pop, const char *service_name, uint16_t default_gateway_port);
bool fx_wifi_connected(void);
bool fx_wifi_onboarding(void);
/* Join a network given at the console, ending onboarding if it is running. Kept for later boots. */
esp_err_t fx_wifi_set_credentials(const char *ssid, const char *password);
/* Erase the stored network. Takes effect at the next boot. */
esp_err_t fx_wifi_forget(void);
/* One line for the console: network, address and signal. */
void fx_wifi_describe(char *out, size_t cap);

/* --- provisioning client -------------------------------------------------------- */

typedef enum {
    FX_PROV_OK = 0,
    FX_PROV_NETWORK,      /* no answer from the gateway */
    FX_PROV_REJECTED,     /* 400 */
    FX_PROV_NO_PAIRING,   /* 403 */
    FX_PROV_CONFLICT,     /* 409 */
    FX_PROV_RATE_LIMITED, /* 429 */
    FX_PROV_BAD_REPLY,    /* an answer that is not a provisioning reply */
    FX_PROV_BAD_MAC,      /* a reply not made with this unit's pairing code */
    FX_PROV_INTERNAL
} fx_prov_outcome_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *code; /* normalised pairing code */
    const char *fw_version;
    const uint8_t *device_id;  /* 16 bytes */
    const uint8_t *ledger_pub; /* 65 bytes */
    fx_signer_t ledger_signer;
    const fx_tls_key_t *tls;
} fx_prov_client_t;

/* One attempt: POST /api/provision and check the reply MAC. On FX_PROV_OK `out`
 * holds the credentials to store, and to free. */
fx_prov_outcome_t fx_prov_run(const fx_prov_client_t *c, fx_net_creds_t *out, int *http_status);
const char *fx_prov_outcome_text(fx_prov_outcome_t o);

#ifdef __cplusplus
}
#endif
#endif /* FX_NET_HW_H */
