/*
 * Reporter: the M1 serial stream for tools/capture, fan control and fan
 * health, and the results of clean-air baseline captures.
 *
 *   H,<column names>      when a stream starts
 *   S,<t_ms>,<values>     one row per period
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "node.h"

static const char *const SCORE_COLUMNS =
    "gas,gas_ok,smoke,smoke_ok,flame,flame_ok,heat,heat_ok,rh,rh_ok,fan_duty,fan_rpm,fan_state,alarm";

void node_stream(bool on, uint32_t period_ms)
{
    portENTER_CRITICAL(&g_node.stream_lock);
    g_node.streaming = on;
    g_node.header_pending = on;
    if (period_ms > 0u) {
        g_node.stream_period_ms = period_ms;
    }
    portEXIT_CRITICAL(&g_node.stream_lock);
}

static void print_header(void)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "H,t_ms");
    for (int i = 0; i < NODE_SENSOR_COUNT && n < (int)sizeof(line); i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, ",%s", g_node.sensors[i]->raw_header);
    }
    if (n < (int)sizeof(line)) {
        snprintf(line + n, sizeof(line) - (size_t)n, ",%s", SCORE_COLUMNS);
    }
    printf("%s\n", line);
    fflush(stdout);
}

static void print_sample(uint32_t now_ms)
{
    char line[600];
    int n = snprintf(line, sizeof(line), "S,%" PRIu32, now_ms);
    for (int i = 0; i < NODE_SENSOR_COUNT && n < (int)sizeof(line) - 2; i++) {
        fx_sensor_t *s = g_node.sensors[i];
        line[n++] = ',';
        line[n] = '\0';
        if (s->ready) {
            const int w = s->raw(s, line + n, sizeof(line) - (size_t)n);
            n += w > 0 ? w : 0;
        } else {
            for (const char *c = s->raw_header; *c && n < (int)sizeof(line) - 1; c++) {
                if (*c == ',') {
                    line[n++] = ',';
                }
            }
            line[n] = '\0';
        }
    }

    fx_sample_t samples[FX_MODALITY_COUNT];
    portENTER_CRITICAL(&g_node.state_lock);
    memcpy(samples, g_node.samples, sizeof(samples));
    const fx_alarm_state_t alarm = g_node.alarm_state;
    portEXIT_CRITICAL(&g_node.state_lock);
    for (int m = 0; m < FX_MODALITY_COUNT && n < (int)sizeof(line); m++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, ",%.3f,%d", (double)samples[m].score,
                      samples[m].valid ? 1 : 0);
    }
    const fx_fan_status_t fan = fx_fan_status(&g_node.fan, now_ms);
    if (n < (int)sizeof(line)) {
        snprintf(line + n, sizeof(line) - (size_t)n, ",%u,%" PRIu32 ",%s,%s", fan.duty_pct, fan.rpm,
                 g_node.fan_ok ? fx_fan_state_name(fan.state) : "absent", fx_alarm_state_name(alarm));
    }
    printf("%s\n", line);
    fflush(stdout);
}

void node_print_snapshot(void)
{
    const uint32_t now = node_now_ms();
    fx_sample_t samples[FX_MODALITY_COUNT];
    float effective[FX_MODALITY_COUNT];
    portENTER_CRITICAL(&g_node.state_lock);
    memcpy(samples, g_node.samples, sizeof(samples));
    memcpy(effective, g_node.effective, sizeof(effective));
    const fx_state_t fusion = g_node.fusion_state;
    const fx_alarm_state_t alarm = g_node.alarm_state;
    portEXIT_CRITICAL(&g_node.state_lock);

    printf("t = %" PRIu32 " ms, fusion %s, alarm %s\n", now, fx_state_name(fusion), fx_alarm_state_name(alarm));
    for (int m = 0; m < FX_MODALITY_COUNT; m++) {
        printf("  %-9s score %.3f  effective %.3f  %s\n", fx_modality_key((fx_modality_t)m),
               (double)samples[m].score, (double)effective[m], samples[m].valid ? "valid" : "INVALID");
    }
    char raw[160];
    for (int i = 0; i < NODE_SENSOR_COUNT; i++) {
        fx_sensor_t *s = g_node.sensors[i];
        if (s->ready) {
            s->raw(s, raw, sizeof(raw));
        } else {
            snprintf(raw, sizeof(raw), "not ready");
        }
        printf("  %-6s [%s] %s\n", s->name, s->raw_header, raw);
    }
    const fx_fan_status_t fan = fx_fan_status(&g_node.fan, now);
    printf("  fan    duty %u%%  rpm %" PRIu32 "  %s%s\n", fan.duty_pct, fan.rpm,
           g_node.fan_ok ? fx_fan_state_name(fan.state) : "absent", fan.tach ? "" : " (no tachometer)");
}

static void emit_baseline(fx_mq_driver_t *d, const char *setting_key)
{
    fx_mq_baseline_status_t st;
    float r0 = 0.0f, spread = 0.0f;
    if (!fx_mq_driver_take_baseline(d, &st, &r0, &spread)) {
        return;
    }
    if (st != FX_MQ_BASELINE_READY) {
        printf("%s baseline UNSTABLE: spread %.2f%% over %u readings; R0 unchanged\n", d->sensor.name,
               (double)(spread * 100.0f), g_node.baseline_samples);
        return;
    }
    const uint32_t ohms = (uint32_t)(r0 + 0.5f);
    const char *why = NULL;
    if (node_setting_change(setting_key, ohms, false, &why) != FX_SET_OK) {
        printf("%s baseline ready but not stored: %s\n", d->sensor.name, why ? why : "?");
        return;
    }
    uint8_t payload[FX_PAYLOAD_MAX];
    const size_t len = fx_ev_calibration(payload, d->modality, ohms, "clean_air_median");
    node_log("calibration", payload, len);
    printf("%s R0 = %" PRIu32 " ohm (spread %.2f%%), stored\n", d->sensor.name, ohms, (double)(spread * 100.0f));
}

static void reporter_task(void *arg)
{
    (void)arg;
    uint32_t last_sample_ms = 0;
    uint8_t applied_duty = 255;
    fx_fan_state_t logged_state = FX_FAN_OFF;
    uint8_t logged_duty = 255;
    uint8_t payload[FX_PAYLOAD_MAX];
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        const uint32_t now = node_now_ms();

        if (g_node.fan_ok) {
            const fx_settings_t s = node_settings_snapshot(NULL);
            const uint8_t desired = g_node.fan_test ? 100u
                                    : g_node.fan_override <= 100u ? g_node.fan_override
                                                                   : (uint8_t)s.v[FX_S_FAN_DUTY];
            if (desired != applied_duty && fx_fan_set_duty(&g_node.fan, desired, now) == ESP_OK) {
                applied_duty = desired;
            }
            fx_fan_poll(&g_node.fan, now);
            const fx_fan_status_t fan = fx_fan_status(&g_node.fan, now);
            if (!g_node.fan_test && (fan.state != logged_state || fan.duty_pct != logged_duty)) {
                const size_t len = fx_ev_fan_state(payload, fx_fan_state_name(fan.state), fan.duty_pct,
                                                   fan.tach ? "rpm" : NULL, fan.rpm);
                node_log("fan_state", payload, len);
                logged_state = fan.state;
                logged_duty = fan.duty_pct;
            }
        }

        emit_baseline(&g_node.mq2, "mq2_r0");
        emit_baseline(&g_node.mq135, "mq135_r0");

        portENTER_CRITICAL(&g_node.stream_lock);
        const bool streaming = g_node.streaming;
        const bool header = g_node.header_pending;
        g_node.header_pending = false;
        const uint32_t period = g_node.stream_period_ms;
        portEXIT_CRITICAL(&g_node.stream_lock);

        if (header) {
            print_header();
            last_sample_ms = now - period;
        }
        if (streaming && now - last_sample_ms >= period) {
            print_sample(now);
            last_sample_ms = now;
        }
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(50));
    }
}

void node_reporter_start(void)
{
    xTaskCreate(reporter_task, "reporter", 6144, NULL, 3, NULL);
}
