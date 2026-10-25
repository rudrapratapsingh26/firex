/*
 * Fusion on the node: every 500 ms, collect the five samples through the score
 * API, run the engine, write threshold crossings, state changes and sensor
 * faults to the ledger, publish the state for the alarm task, and wake the
 * camera task on the transition into CRITICAL.
 *
 * Settings changes apply to the running engine without resetting it, so a
 * threshold tweak never restarts the warm-up.
 */
#include <string.h>

#include "esp_task_wdt.h"
#include "node.h"

#define PERIOD_MS 500u
#define FAULT_GRACE_MS 15000u /* sensors are still producing their first readings */

static uint8_t s_payload[FX_PAYLOAD_MAX];

static void apply_settings(fx_engine_t *eng, const fx_settings_t *s)
{
    fx_config_t cfg;
    fx_settings_fusion(s, &cfg);
    /* A reset that kept the MQ heaters powered (an update's reboot, a watchdog)
     * needs only the short warm-up; power-on and brownout get the whole one. */
    cfg.warmup_ms = fx_ota_warmup_ms(s->v[FX_S_WARMUP_S], s->v[FX_S_WARM_S], g_node.warm_start);
    eng->cfg = cfg;

    fx_mq_driver_set_r0(&g_node.mq2, fx_settings_r0(s, FX_GAS));
    fx_mq_driver_set_r0(&g_node.mq135, fx_settings_r0(s, FX_SMOKE));
    fx_mq_driver_set_curve(&g_node.mq2, (float)s->v[FX_S_MQ2_FULL] / 100.0f);
    fx_mq_driver_set_curve(&g_node.mq135, (float)s->v[FX_S_MQ135_FULL] / 100.0f);

    fx_heat_params_t heat;
    fx_settings_heat(s, &heat);
    fx_dht22_driver_set_params(&g_node.dht, &heat);
#if CONFIG_FIREX_HEAT_NTC
    fx_ntc_driver_set_params(&g_node.ntc, &heat);
#endif
    fx_flame_driver_set_thresholds(&g_node.flame, (float)s->v[FX_S_FLAME_QUIET], (float)s->v[FX_S_FLAME_FULL]);
}

static void fusion_task(void *arg)
{
    (void)arg;
    uint32_t version = 0;
    fx_settings_t settings = node_settings_snapshot(&version);
    fx_engine_t eng;
    fx_config_t cfg;
    fx_settings_fusion(&settings, &cfg);
    const uint32_t boot_ms = node_now_ms();
    fx_init(&eng, &cfg, boot_ms);
    apply_settings(&eng, &settings);

    bool prev_active[FX_MODALITY_COUNT] = {false};
    bool prev_valid[FX_MODALITY_COUNT] = {false};
    bool faults_armed = false;
    fx_state_t last_state = FX_STATE_WARMUP;

    esp_task_wdt_add(NULL);
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        esp_task_wdt_reset();

        uint32_t v = 0;
        settings = node_settings_snapshot(&v);
        if (v != version) {
            apply_settings(&eng, &settings);
            version = v;
        }

        const uint32_t now = node_now_ms();
        fx_sample_t samples[FX_MODALITY_COUNT];
        fx_sensors_collect(g_node.sensors, NODE_SENSOR_COUNT, now, samples);

        portENTER_CRITICAL(&g_node.state_lock);
        const uint8_t mask = g_node.selftest_mask;
        portEXIT_CRITICAL(&g_node.state_lock);

        bool valid[FX_MODALITY_COUNT];
        bool unexplained_invalid = false;
        for (int m = 0; m < FX_MODALITY_COUNT; m++) {
            if (mask & FX_MOD_BIT(m)) {
                samples[m].valid = false; /* excluded for a self-test loopback, not failed */
                samples[m].score = 0.0f;
            }
            valid[m] = samples[m].valid;
            if (!valid[m] && !(mask & FX_MOD_BIT(m))) {
                unexplained_invalid = true;
            }
        }

        const fx_result_t r = fx_update(&eng, samples, now);

        fx_crossing_t crossings[FX_MODALITY_COUNT];
        const size_t n = fx_detect_crossings(prev_active, eng.active, valid, r.effective, crossings);
        for (size_t i = 0; i < n; i++) {
            const size_t len = fx_ev_threshold_cross(s_payload, crossings[i].modality, crossings[i].score, crossings[i].up);
            node_log("threshold_cross", s_payload, len);
        }

        if (r.state_changed) {
            fx_modality_t reasons[2];
            size_t nr = 0;
            if (r.reason_a < FX_MODALITY_COUNT) {
                reasons[nr++] = r.reason_a;
            }
            if (r.reason_b < FX_MODALITY_COUNT) {
                reasons[nr++] = r.reason_b;
            }
            const size_t len = fx_ev_state_change(s_payload, last_state, r.state, reasons, nr, r.effective, valid);
            node_log("state_change", s_payload, len);
            if (r.state == FX_STATE_CRITICAL && g_node.camera_task) {
                xTaskNotifyGive(g_node.camera_task);
            }
            last_state = r.state;
        }

        /* Sensor faults, once every driver has had time to produce a reading. */
        if (!faults_armed && now - boot_ms >= FAULT_GRACE_MS) {
            faults_armed = true;
            for (int m = 0; m < FX_MODALITY_COUNT; m++) {
                prev_valid[m] = true; /* report anything already invalid as raised */
            }
        }
        if (faults_armed) {
            for (int m = 0; m < FX_MODALITY_COUNT; m++) {
                if (mask & FX_MOD_BIT(m)) {
                    continue;
                }
                if (valid[m] != prev_valid[m]) {
                    const size_t len = fx_ev_fault(s_payload, fx_modality_key((fx_modality_t)m), !valid[m]);
                    node_log("fault", s_payload, len);
                    prev_valid[m] = valid[m];
                }
            }
        }

        portENTER_CRITICAL(&g_node.state_lock);
        g_node.have_fusion = true;
        g_node.fusion_loops++; /* a new image's confirm check watches this advance */
        g_node.fusion_state = r.state;
        g_node.sensor_fault = faults_armed && unexplained_invalid;
        memcpy(g_node.samples, samples, sizeof(samples));
        memcpy(g_node.effective, r.effective, sizeof(r.effective));
        portEXIT_CRITICAL(&g_node.state_lock);

        xTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
    }
}

void node_fusion_start(void)
{
    xTaskCreate(fusion_task, "fusion", 6144, NULL, 8, NULL);
}
