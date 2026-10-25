/*
 * Daily self-test. Runs only while the node is NORMAL, never delays an alarm,
 * and writes one self_test entry with every check that ran. A failed check
 * holds the FAULT indication until a later self-test passes.
 *
 * The flame loopback makes the flame sensor see flicker, so for its few seconds
 * fusion is told to ignore the flame channel. Gas, smoke and heat keep voting
 * throughout; a hazard that appears mid-test ends the test.
 *
 * A bench image (CONFIG_FIREX_BENCH_INJECT) also runs flame injections here,
 * because this task owns the IR LED: the same flicker, unmasked, so the real
 * sensor, fusion, alarm, camera and ledger path answers it. Running both in one
 * task means an injection and a self-test can never overlap.
 */
#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_system.h"
#include "node.h"
#include "sdkconfig.h"

#define POLL_MS 10000u
#define HEAP_FLOOR 30000u
#define LOOPBACK_HALF_PERIOD_MS 62u /* 8 Hz */
#define LOOPBACK_TOGGLES 48u        /* 3 s */

static bool hazard(void)
{
    portENTER_CRITICAL(&g_node.state_lock);
    const bool h = g_node.fusion_state == FX_STATE_ELEVATED || g_node.fusion_state == FX_STATE_CRITICAL;
    portEXIT_CRITICAL(&g_node.state_lock);
    return h;
}

static bool quiet(void)
{
    portENTER_CRITICAL(&g_node.state_lock);
    const bool q = g_node.have_fusion && g_node.fusion_state == FX_STATE_NORMAL;
    portEXIT_CRITICAL(&g_node.state_lock);
    return q;
}

static void set_mask(uint8_t mask)
{
    portENTER_CRITICAL(&g_node.state_lock);
    g_node.selftest_mask = mask;
    portEXIT_CRITICAL(&g_node.state_lock);
}

static void run_self_test(void)
{
    fx_selftest_report_t rep;
    fx_selftest_begin(&rep);
    portENTER_CRITICAL(&g_node.state_lock);
    g_node.self_test_running = true;
    portEXIT_CRITICAL(&g_node.state_lock);
    printf("self-test started\n");

    /* heater: plausible warm output in clean air, both MQ sensors */
    if (g_node.mq2.sensor.ready && g_node.mq135.sensor.ready) {
        const uint32_t now = node_now_ms();
        float rs = 0, r0 = 0;
        const bool mq2 = fx_mq_driver_latest(&g_node.mq2, now, &rs, &r0) && fx_check_heater(true, rs, r0);
        const bool mq135 = fx_mq_driver_latest(&g_node.mq135, now, &rs, &r0) && fx_check_heater(true, rs, r0);
        fx_selftest_add(&rep, "heater", mq2 && mq135);
    }

    /* buzzer: a 150 ms chirp through the alarm task, which owns the buzzer */
    portENTER_CRITICAL(&g_node.state_lock);
    g_node.chirp_until_ms = node_now_ms() + 150u;
    portEXIT_CRITICAL(&g_node.state_lock);
    vTaskDelay(pdMS_TO_TICKS(300));
    portENTER_CRITICAL(&g_node.state_lock);
    const bool buzzer_ok = g_node.buzzer.ready && !g_node.buzzer_error;
    portEXIT_CRITICAL(&g_node.state_lock);
    fx_selftest_add(&rep, "buzzer", buzzer_ok);

    /* flame loopback */
    const int led = CONFIG_FIREX_FLAME_TEST_LED_GPIO;
    if (led >= 0 && g_node.flame.sensor.ready && !hazard()) {
        set_mask(FX_MOD_BIT(FX_FLAME));
        vTaskDelay(pdMS_TO_TICKS(100));
        const float before = fx_flame_driver_result(&g_node.flame).score;
        const gpio_config_t io = {.pin_bit_mask = 1ULL << led, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&io);
        float during = 0.0f;
        for (uint32_t k = 0; k < LOOPBACK_TOGGLES; k++) {
            gpio_set_level(led, (int)(k & 1u));
            vTaskDelay(pdMS_TO_TICKS(LOOPBACK_HALF_PERIOD_MS));
            const float score = fx_flame_driver_result(&g_node.flame).score;
            during = score > during ? score : during;
        }
        gpio_set_level(led, 0);
        vTaskDelay(pdMS_TO_TICKS(3000)); /* let the 2 s flicker window forget the LED */
        set_mask(0);
        fx_selftest_add(&rep, "flame_loopback", fx_check_flame_loopback(before, during));
    }

    /* fan: full duty, then the tachometer must agree */
    if (g_node.fan_ok && g_node.fan.pcnt && !hazard()) {
        g_node.fan_test = true;
        vTaskDelay(pdMS_TO_TICKS(6000));
        const fx_fan_status_t st = fx_fan_status(&g_node.fan, node_now_ms());
        g_node.fan_test = false;
        fx_selftest_add(&rep, "fan", st.state == FX_FAN_ON && st.rpm >= g_node.fan.params.min_rpm);
    }

    /* camera: a capture whose image never leaves the camera */
    const fx_settings_t s = node_settings_snapshot(NULL);
    if (g_node.cam_ok && s.v[FX_S_CAM_FRAMES] > 0 && !hazard()) {
        const fx_cam_result_t res = fx_cam_check(&g_node.cam, 8000);
        fx_selftest_add(&rep, "camera", res.status == FX_FRAME_CHECK && g_node.cam.rx.check_ok);
    }

    /* ledger: every stored entry links and hashes; the newest one verifies */
    fx_chain_report_t chain;
    const bool ledger_ok = g_node.ledger_ok && node_ledger_check(false, &chain) && node_ledger_verify_newest();
    fx_selftest_add(&rep, "ledger", ledger_ok);

    fx_selftest_add(&rep, "heap", fx_check_heap(esp_get_free_heap_size(), HEAP_FLOOR));
    if (hazard()) {
        fx_selftest_add(&rep, "completed", false);
    }

    const bool pass = fx_selftest_pass(&rep);
    uint8_t payload[FX_PAYLOAD_MAX];
    const size_t len = fx_ev_self_test(payload, pass, rep.checks, rep.n);
    node_log("self_test", payload, len);

    portENTER_CRITICAL(&g_node.state_lock);
    g_node.selftest_fault = !pass;
    g_node.self_test_running = false;
    g_node.selftest_mask = 0;
    portEXIT_CRITICAL(&g_node.state_lock);

    printf("self-test %s:", pass ? "PASS" : "FAIL");
    for (size_t i = 0; i < rep.n; i++) {
        printf(" %s=%s", rep.checks[i].name, rep.checks[i].ok ? "ok" : "FAIL");
    }
    printf("\n");
}

#if CONFIG_FIREX_BENCH_INJECT
bool node_inject_flame(uint32_t seconds, const char **why)
{
    const int led = CONFIG_FIREX_FLAME_TEST_LED_GPIO;
    const char *reason = NULL;
    if (led < 0) {
        reason = "no IR LED is fitted (FIREX_FLAME_TEST_LED_GPIO)";
    } else if (!g_node.flame.sensor.ready || !g_node.selftest_task) {
        reason = "the flame sensor is not ready";
    } else {
        portENTER_CRITICAL(&g_node.state_lock);
        if (!g_node.have_fusion || g_node.fusion_state != FX_STATE_NORMAL) {
            reason = "the node is not in NORMAL";
        } else if (g_node.self_test_running) {
            reason = "a self-test is running";
        } else if (g_node.injecting) {
            reason = "an injection is already running";
        } else {
            g_node.inject_seconds = seconds;
            g_node.injecting = true;
        }
        portEXIT_CRITICAL(&g_node.state_lock);
    }
    if (reason) {
        if (why) {
            *why = reason;
        }
        return false;
    }
    xTaskNotifyGive(g_node.selftest_task);
    return true;
}

static void echo_inject(uint32_t seconds, const char *phase)
{
    char body[96];
    snprintf(body, sizeof(body), "\"type\":\"inject\",\"source\":\"flame\",\"seconds\":%" PRIu32 ",\"phase\":\"%s\"",
             seconds, phase);
    node_echo_event(body);
}

static void run_injection(uint32_t seconds)
{
    const int led = CONFIG_FIREX_FLAME_TEST_LED_GPIO;
    echo_inject(seconds, "start");
    const gpio_config_t io = {.pin_bit_mask = 1ULL << led, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    const uint32_t toggles = seconds * 1000u / LOOPBACK_HALF_PERIOD_MS;
    for (uint32_t k = 0; k < toggles; k++) {
        gpio_set_level(led, (int)(k & 1u));
        vTaskDelay(pdMS_TO_TICKS(LOOPBACK_HALF_PERIOD_MS));
    }
    gpio_set_level(led, 0);
    echo_inject(seconds, "end");
    portENTER_CRITICAL(&g_node.state_lock);
    g_node.injecting = false;
    portEXIT_CRITICAL(&g_node.state_lock);
}
#endif

static void selftest_task(void *arg)
{
    (void)arg;
    bool have_run = false;
    uint32_t last_run = 0;
    for (;;) {
        const bool manual = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS)) > 0;
#if CONFIG_FIREX_BENCH_INJECT
        portENTER_CRITICAL(&g_node.state_lock);
        const uint32_t inject_s = g_node.inject_seconds;
        g_node.inject_seconds = 0;
        portEXIT_CRITICAL(&g_node.state_lock);
        if (inject_s > 0u) {
            run_injection(inject_s);
            continue; /* that notification was the injection, not a self-test request */
        }
#endif
        const fx_settings_t s = node_settings_snapshot(NULL);
        const uint32_t now = node_now_ms();
        const bool due = fx_selftest_due(have_run, last_run, now, s.v[FX_S_SELFTEST_H],
                                         s.v[FX_S_WARMUP_S] * 1000u + 60000u);
        if (!manual && !due) {
            continue;
        }
        if (!quiet()) {
            if (manual) {
                printf("self-test refused: the node is not in NORMAL\n");
            }
            continue; /* a due test retries at the next poll */
        }
        run_self_test();
        have_run = true;
        last_run = node_now_ms();
    }
}

void node_selftest_start(void)
{
    xTaskCreate(selftest_task, "selftest", 8192, NULL, 2, &g_node.selftest_task);
}
