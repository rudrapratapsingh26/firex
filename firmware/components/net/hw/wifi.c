#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "fx_net_hw.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

#define CONNECTED_BIT BIT0
#define GATEWAY_ENDPOINT "custom-data" /* where esp_prov --custom_data sends */

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static volatile bool s_started;
static volatile bool s_onboarding;
static uint16_t s_default_port;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "onboarding: connect to this unit's access point with the ESP SoftAP Provisioning app");
            break;
        case WIFI_PROV_CRED_RECV: {
            const wifi_sta_config_t *cfg = (const wifi_sta_config_t *)data;
            ESP_LOGI(TAG, "onboarding: received the network \"%.32s\"", (const char *)cfg->ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const wifi_prov_sta_fail_reason_t *reason = (const wifi_prov_sta_fail_reason_t *)data;
            ESP_LOGW(TAG, "onboarding: could not join (%s); send the network again",
                     *reason == WIFI_PROV_STA_AUTH_ERROR ? "wrong password" : "network not found");
            wifi_prov_mgr_reset_sm_state_on_failure();
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "onboarding: joined the site network");
            break;
        case WIFI_PROV_END:
            wifi_prov_mgr_deinit();
            s_onboarding = false;
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START && !s_onboarding) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_events, CONNECTED_BIT);
            if (!s_onboarding) {
                esp_wifi_connect(); /* each attempt includes a scan, so this is not a busy loop */
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, address " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

/* The onboarding app sends the gateway's host[:port] here after the network. */
static esp_err_t gateway_endpoint(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf,
                                  ssize_t *outlen, void *priv)
{
    (void)session_id;
    (void)priv;
    const char *reply = "error: send host or host:port";
    char text[96], host[FX_NET_HOST_MAX];
    uint16_t port = 0;
    if (inbuf && inlen > 0 && inlen < (ssize_t)sizeof(text)) {
        memcpy(text, inbuf, (size_t)inlen);
        text[inlen] = '\0';
        if (fx_net_parse_host_port(text, s_default_port, host, &port)) {
            if (fx_net_gateway_set(host, port) == ESP_OK) {
                ESP_LOGI(TAG, "onboarding: gateway %s port %u", host, (unsigned)port);
                reply = "ok";
            } else {
                reply = "error: could not store the gateway";
            }
        }
    }
    *outbuf = (uint8_t *)strdup(reply);
    if (!*outbuf) {
        return ESP_ERR_NO_MEM;
    }
    *outlen = (ssize_t)strlen(reply) + 1;
    return ESP_OK;
}

esp_err_t fx_wifi_start(const char *pop, const char *service_name, uint16_t default_gateway_port)
{
    if (s_started) {
        return ESP_OK;
    }
    s_default_port = default_gateway_port;
    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    err = wifi_prov_mgr_init(cfg);
    if (err != ESP_OK) {
        return err;
    }
    bool provisioned = false;
    wifi_prov_mgr_is_provisioned(&provisioned);
    s_started = true;

    if (!provisioned) {
        s_onboarding = true;
        err = wifi_prov_mgr_endpoint_create(GATEWAY_ENDPOINT);
        if (err == ESP_OK) {
            err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, (const void *)pop, service_name, NULL);
        }
        if (err == ESP_OK) {
            err = wifi_prov_mgr_endpoint_register(GATEWAY_ENDPOINT, gateway_endpoint, NULL);
        }
        if (err != ESP_OK) {
            s_onboarding = false;
            wifi_prov_mgr_deinit();
        }
        return err;
    }

    wifi_prov_mgr_deinit();
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    return err;
}

bool fx_wifi_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & CONNECTED_BIT);
}

bool fx_wifi_onboarding(void)
{
    return s_onboarding;
}

esp_err_t fx_wifi_set_credentials(const char *ssid, const char *password)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t ssid_len = ssid ? strlen(ssid) : 0u;
    const size_t pass_len = password ? strlen(password) : 0u;
    if (ssid_len == 0 || ssid_len > 32u || pass_len > 63u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_onboarding) {
        wifi_prov_mgr_stop_provisioning();
        for (int i = 0; i < 50 && s_onboarding; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    memcpy(wc.sta.ssid, ssid, ssid_len);
    if (pass_len) {
        memcpy(wc.sta.password, password, pass_len);
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        esp_wifi_disconnect(); /* the disconnect handler joins with the new settings */
        esp_wifi_connect();
    }
    return err;
}

esp_err_t fx_wifi_forget(void)
{
    return s_started ? esp_wifi_restore() : ESP_ERR_INVALID_STATE;
}

void fx_wifi_describe(char *out, size_t cap)
{
    if (!s_started) {
        snprintf(out, cap, "Wi-Fi is not started");
        return;
    }
    if (s_onboarding) {
        snprintf(out, cap, "onboarding: waiting for the provisioning app, or `wifi set`");
        return;
    }
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    if (fx_wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK && s_sta && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        snprintf(out, cap, "joined \"%.32s\", address " IPSTR ", signal %d dBm", (const char *)ap.ssid, IP2STR(&ip.ip),
                 ap.rssi);
        return;
    }
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
        snprintf(out, cap, "trying to join \"%.32s\"", (const char *)wc.sta.ssid);
    } else {
        snprintf(out, cap, "no network set: use `wifi set` or the provisioning app");
    }
}
