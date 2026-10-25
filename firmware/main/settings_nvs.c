/*
 * Settings in NVS. One u32 per key in namespace "fx_cfg"; a missing key reads
 * as its default. Every change made while running is a config_change ledger
 * entry carrying the old and new value.
 */
#include <string.h>

#include "esp_log.h"
#include "node.h"
#include "nvs.h"

static const char *TAG = "settings";
#define NS "fx_cfg"

void node_settings_load(void)
{
    fx_settings_t s;
    fx_settings_defaults(&s);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < FX_SETTINGS_COUNT; i++) {
            uint32_t v;
            if (nvs_get_u32(h, FX_SETTING_DEFS[i].key, &v) == ESP_OK && v >= FX_SETTING_DEFS[i].min &&
                v <= FX_SETTING_DEFS[i].max) {
                s.v[i] = v;
            }
        }
        nvs_close(h);
    }
    const char *why = NULL;
    if (!fx_settings_consistent(&s, &why)) {
        ESP_LOGE(TAG, "stored settings are inconsistent (%s); using defaults", why ? why : "?");
        fx_settings_defaults(&s);
    }
    portENTER_CRITICAL(&g_node.settings_lock);
    g_node.settings = s;
    g_node.settings_version++;
    portEXIT_CRITICAL(&g_node.settings_lock);
}

fx_settings_t node_settings_snapshot(uint32_t *version)
{
    portENTER_CRITICAL(&g_node.settings_lock);
    const fx_settings_t s = g_node.settings;
    if (version) {
        *version = g_node.settings_version;
    }
    portEXIT_CRITICAL(&g_node.settings_lock);
    return s;
}

fx_set_status_t node_setting_change(const char *key, uint32_t value, bool log, const char **why)
{
    fx_settings_t trial = node_settings_snapshot(NULL);
    uint32_t old = 0;
    const fx_set_status_t st = fx_settings_set(&trial, key, value, &old, why);
    if (st != FX_SET_OK) {
        return st;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        if (why) {
            *why = "could not write NVS";
        }
        return FX_SET_INCONSISTENT;
    }

    portENTER_CRITICAL(&g_node.settings_lock);
    g_node.settings = trial;
    g_node.settings_version++;
    portEXIT_CRITICAL(&g_node.settings_lock);

    if (log && old != value) {
        uint8_t payload[FX_PAYLOAD_MAX];
        const size_t n = fx_ev_config_change(payload, key, old, value);
        node_log("config_change", payload, n);
    }
    return FX_SET_OK;
}

uint32_t node_boot_count_next(void)
{
    uint32_t count = 0;
    nvs_handle_t h;
    if (nvs_open("fx_sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot_count", &count);
        count++;
        nvs_set_u32(h, "boot_count", count);
        nvs_commit(h);
        nvs_close(h);
    }
    return count;
}
