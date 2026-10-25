#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "fx_net_hw.h"
#include "nvs.h"

#define NS "fx_net"

static const char *TAG = "net_store";

esp_err_t fx_net_pairing_code(char code[11])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char stored[24];
    size_t len = sizeof(stored);
    err = nvs_get_str(h, "pair_code", stored, &len);
    if (err == ESP_OK) {
        nvs_close(h);
        if (fx_pairing_code_normalise(stored, code)) {
            return ESP_OK;
        }
        /* The code is printed on the label: a different one would never match it again. */
        ESP_LOGE(TAG, "the stored pairing code is invalid; refusing to replace it");
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }

    uint8_t random[FX_PAIRING_CODE_LEN];
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();
    fx_pairing_code_from_random(random, code);
    memset(random, 0, sizeof(random));

    err = nvs_set_str(h, "pair_code", code);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool fx_net_parse_host_port(const char *text, uint16_t default_port, char host[FX_NET_HOST_MAX], uint16_t *port)
{
    if (!text) {
        return false;
    }
    char buf[96];
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    size_t n = strlen(text);
    if (n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, text, n + 1u);
    while (n > 0 && (buf[n - 1u] == ' ' || buf[n - 1u] == '\t' || buf[n - 1u] == '\r' || buf[n - 1u] == '\n')) {
        buf[--n] = '\0';
    }

    unsigned long p = default_port;
    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = '\0';
        const char *digits = colon + 1;
        if (!isdigit((unsigned char)digits[0])) {
            return false;
        }
        char *end = NULL;
        p = strtoul(digits, &end, 10);
        if (*end != '\0') {
            return false;
        }
    }
    const size_t len = strlen(buf);
    if (len == 0 || len >= FX_NET_HOST_MAX || p == 0 || p > 65535ul || buf[0] == '-' || buf[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = buf[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '-') {
            return false;
        }
    }
    memcpy(host, buf, len + 1u);
    *port = (uint16_t)p;
    return true;
}

esp_err_t fx_net_gateway_get(char host[FX_NET_HOST_MAX], uint16_t *port)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = FX_NET_HOST_MAX;
    err = nvs_get_str(h, "gw_host", host, &len);
    if (err == ESP_OK) {
        err = nvs_get_u16(h, "gw_port", port);
    }
    nvs_close(h);
    return err;
}

esp_err_t fx_net_gateway_set(const char *host, uint16_t port)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "gw_host", host);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "gw_port", port);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static char *get_alloc_str(nvs_handle_t h, const char *key, esp_err_t *err)
{
    size_t len = 0;
    *err = nvs_get_str(h, key, NULL, &len);
    if (*err != ESP_OK) {
        return NULL;
    }
    char *s = malloc(len);
    if (!s) {
        *err = ESP_ERR_NO_MEM;
        return NULL;
    }
    *err = nvs_get_str(h, key, s, &len);
    if (*err != ESP_OK) {
        free(s);
        return NULL;
    }
    return s;
}

void fx_net_creds_free(fx_net_creds_t *c)
{
    if (!c) {
        return;
    }
    free(c->ca_pem);
    free(c->cert_pem);
    c->ca_pem = NULL;
    c->cert_pem = NULL;
}

esp_err_t fx_net_creds_load(fx_net_creds_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* site_id is written last: without it the rest may be a half-finished store. */
    size_t len = sizeof(c->site_id);
    err = nvs_get_str(h, "site_id", c->site_id, &len);
    if (err == ESP_OK) {
        len = sizeof(c->mqtt_host);
        err = nvs_get_str(h, "mqtt_host", c->mqtt_host, &len);
    }
    if (err == ESP_OK) {
        err = nvs_get_u16(h, "mqtt_port", &c->mqtt_port);
    }
    if (err == ESP_OK) {
        c->ca_pem = get_alloc_str(h, "ca_pem", &err);
    }
    if (err == ESP_OK) {
        c->cert_pem = get_alloc_str(h, "cert_pem", &err);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        fx_net_creds_free(c);
    }
    return err;
}

static void erase_key(nvs_handle_t h, const char *key)
{
    const esp_err_t err = nvs_erase_key(h, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "erase %s: %s", key, esp_err_to_name(err));
    }
}

esp_err_t fx_net_creds_store(const fx_net_creds_t *c)
{
    if (!c || !c->ca_pem || !c->cert_pem) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    erase_key(h, "site_id");
    err = nvs_commit(h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "mqtt_host", c->mqtt_host);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "mqtt_port", c->mqtt_port);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ca_pem", c->ca_pem);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "cert_pem", c->cert_pem);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "site_id", c->site_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t fx_net_creds_erase(bool gateway)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    erase_key(h, "site_id");
    erase_key(h, "mqtt_host");
    erase_key(h, "mqtt_port");
    erase_key(h, "ca_pem");
    erase_key(h, "cert_pem");
    if (gateway) {
        erase_key(h, "gw_host");
        erase_key(h, "gw_port");
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
