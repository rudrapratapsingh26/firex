/*
 * Console. Every command that changes something is logged.
 *
 *   status                                what is running, faults, ledger and camera
 *   read                                  scores and raw values
 *   stream on [period_ms] | off           S lines for tools/capture
 *   baseline mq2|mq135 [n] [tol] [force]  capture R0 in clean air; stores it and logs calibration
 *   r0 mq2|mq135 <ohms>                   set R0 by hand; logs calibration (method manual)
 *   config list | get <key> | set <key> <value> | reset <key>
 *   fan on | off | <0-100> | auto         bench override of the fan duty
 *   mark <text>                           a timestamped marker line (not a ledger entry)
 *   ledger status | tail [n] | check [sig] | export [from_seq] | key | wipe ERASE-ALL-EVIDENCE
 *   media list | get <name>               get is not in production images: photos leave only to the gateway
 *   selftest                             run the self-test now (only in NORMAL)
 *   cam ping                              is the camera answering
 *   i2cscan
 *   pairing                               this unit's pairing code
 *   wifi status | set <ssid> [password] | forget
 *   gateway show | set <host[:port]>      the gateway to provision with
 *   net status | provision | forget [all] the link to the site gateway
 *   ota status                            firmware slots, confirmation, the last update
 *   clock                                 the ledger clock: its source and the last gateway sync
 *   trace status                          episode traces waiting for the gateway
 *   inject flame <seconds>                bench image only (CONFIG_FIREX_BENCH_INJECT)
 *   se status | provision <token>         the ATECC608B (console_security.c, M6)
 *   security status                       secure boot, flash encryption, debug fuses (M6)
 *
 * There is deliberately no command that silences the alarm or triggers the
 * camera: silence is a physical button, and capture follows CRITICAL only. The
 * bench-only inject does not bypass that: it flickers an IR LED at the real
 * flame sensor, and whatever CRITICAL follows comes out of the normal path.
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "node.h"
#include "sdkconfig.h"

static const char *TAG = "console";

static fx_mq_driver_t *mq_by_name(const char *name)
{
    if (strcmp(name, "mq2") == 0) {
        return &g_node.mq2;
    }
    if (strcmp(name, "mq135") == 0) {
        return &g_node.mq135;
    }
    return NULL;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || !isdigit((unsigned char)s[0])) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const uint32_t now = node_now_ms();
    portENTER_CRITICAL(&g_node.state_lock);
    const fx_state_t fusion = g_node.fusion_state;
    const fx_alarm_state_t alarm = g_node.alarm_state;
    const bool sf = g_node.sensor_fault, lf = g_node.ledger_fault, tf = g_node.selftest_fault, bf = g_node.buzzer_error;
    portEXIT_CRITICAL(&g_node.state_lock);

    printf("FireX node %s, boot %" PRIu32 " (%s), uptime %" PRIu32 " s, free heap %" PRIu32 "\n", g_node.fw_version,
           g_node.boot_count, g_node.reset_reason, now / 1000u, esp_get_free_heap_size());
#if CONFIG_FIREX_BENCH_INJECT
    printf("BENCH IMAGE: `inject flame` is enabled; never fit this image to a unit that can see people\n");
#endif
    printf("fusion %s, alarm %s\n", fx_state_name(fusion), fx_alarm_state_name(alarm));
    printf("faults: sensor %s, ledger %s, self-test %s, buzzer %s\n", sf ? "YES" : "no", lf ? "YES" : "no",
           tf ? "YES" : "no", bf ? "YES" : "no");
    printf("ads1115 %s", g_node.adc_ok ? "ok" : "MISSING");
    for (int i = 0; i < NODE_SENSOR_COUNT; i++) {
        printf(", %s %s", g_node.sensors[i]->name, g_node.sensors[i]->ready ? "ready" : "NOT READY");
    }
    printf("\nbuzzer %s, ring %s, camera link %s, media %s, captures %" PRIu32 "\n",
           g_node.buzzer.ready ? "ok" : "MISSING", g_node.ring.ready ? "ok" : "absent",
           g_node.cam_ok ? "ok" : "MISSING", g_node.media_ok ? "mounted" : "NOT MOUNTED", g_node.captures);
    printf("dht22 reads ok %" PRIu32 ", failed %" PRIu32 "\n", g_node.dht.reads_ok, g_node.dht.reads_failed);
#if CONFIG_FIREX_HEAT_NTC
    printf("ntc reads ok %" PRIu32 ", open/short/out of range %" PRIu32 ", failed %" PRIu32 "\n", g_node.ntc.reads_ok,
           g_node.ntc.reads_out_of_range, g_node.ntc.reads_failed);
#endif
    node_ledger_print_status();
    return 0;
}

static int cmd_read(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    node_print_snapshot();
    return 0;
}

static int cmd_stream(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        uint32_t period = 0;
        if (argc >= 3 && (!parse_u32(argv[2], &period) || period < 100u || period > 60000u)) {
            printf("period must be 100-60000 ms\n");
            return 1;
        }
        node_stream(true, period);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        node_stream(false, 0);
        return 0;
    }
    printf("usage: stream on [period_ms] | off\n");
    return 1;
}

static int cmd_baseline(int argc, char **argv)
{
    fx_mq_driver_t *d = argc >= 2 ? mq_by_name(argv[1]) : NULL;
    if (!d) {
        printf("usage: baseline mq2|mq135 [samples 10-64] [tolerance] [force]\n");
        return 1;
    }
    long samples = 60;
    float tolerance = 0.05f;
    bool force = false;
    int positional = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "force") == 0) {
            force = true;
        } else if (positional++ == 0) {
            samples = strtol(argv[i], NULL, 10);
        } else {
            tolerance = strtof(argv[i], NULL);
        }
    }
    samples = samples < 10 ? 10 : (samples > (long)FX_MQ_BASELINE_MAX ? (long)FX_MQ_BASELINE_MAX : samples);
    if (!d->sensor.ready) {
        printf("%s is not ready; see `status`\n", d->sensor.name);
        return 1;
    }
    const fx_settings_t s = node_settings_snapshot(NULL);
    const uint32_t up_s = node_now_ms() / 1000u;
    if (!force && up_s < s.v[FX_S_WARMUP_S]) {
        printf("heater still warming: %" PRIu32 " s left (add `force` to capture anyway)\n", s.v[FX_S_WARMUP_S] - up_s);
        return 1;
    }
    g_node.baseline_samples = (uint16_t)samples;
    fx_mq_driver_begin_baseline(d, (uint16_t)samples, tolerance);
    printf("capturing %ld readings for %s, about %ld s; keep the air clean\n", samples, d->sensor.name,
           samples * (long)d->sensor.period_ms / 1000);
    return 0;
}

static int cmd_r0(int argc, char **argv)
{
    fx_mq_driver_t *d = argc >= 3 ? mq_by_name(argv[1]) : NULL;
    uint32_t ohms = 0;
    if (!d || !parse_u32(argv[2], &ohms) || ohms == 0) {
        printf("usage: r0 mq2|mq135 <ohms>\n");
        return 1;
    }
    const char *why = NULL;
    if (node_setting_change(d == &g_node.mq2 ? "mq2_r0" : "mq135_r0", ohms, false, &why) != FX_SET_OK) {
        printf("refused: %s\n", why ? why : "?");
        return 1;
    }
    uint8_t payload[FX_PAYLOAD_MAX];
    const size_t len = fx_ev_calibration(payload, d->modality, ohms, "manual");
    node_log("calibration", payload, len);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    const fx_settings_t s = node_settings_snapshot(NULL);
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        for (int i = 0; i < FX_SETTINGS_COUNT; i++) {
            const fx_setting_def_t *d = &FX_SETTING_DEFS[i];
            printf("%-15s %10" PRIu32 " %-10s [%" PRIu32 "-%" PRIu32 "] %s%s\n", d->key, s.v[i], d->unit, d->min,
                   d->max, d->help, s.v[i] != d->def ? " (changed)" : "");
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "get") == 0) {
        const int id = fx_settings_find(argv[2]);
        if (id < 0) {
            printf("unknown key %s\n", argv[2]);
            return 1;
        }
        printf("%s = %" PRIu32 " %s\n", argv[2], s.v[id], FX_SETTING_DEFS[id].unit);
        return 0;
    }
    if ((argc >= 4 && strcmp(argv[1], "set") == 0) || (argc >= 3 && strcmp(argv[1], "reset") == 0)) {
        const int id = fx_settings_find(argv[2]);
        uint32_t value = 0;
        if (id < 0) {
            printf("unknown key %s\n", argv[2]);
            return 1;
        }
        if (strcmp(argv[1], "reset") == 0) {
            value = FX_SETTING_DEFS[id].def;
        } else if (!parse_u32(argv[3], &value)) {
            printf("value must be a non-negative integer\n");
            return 1;
        }
        const char *why = NULL;
        const fx_set_status_t st = node_setting_change(argv[2], value, true, &why);
        if (st != FX_SET_OK) {
            printf("refused: %s\n", why ? why : "out of range");
            return 1;
        }
        printf("%s = %" PRIu32 "\n", argv[2], value);
        return 0;
    }
    printf("usage: config list | get <key> | set <key> <value> | reset <key>\n");
    return 1;
}

static int cmd_fan(int argc, char **argv)
{
    if (!g_node.fan_ok) {
        printf("fan driver did not start; check the PWM GPIO\n");
        return 1;
    }
    uint32_t duty = 0;
    if (argc >= 2 && strcmp(argv[1], "auto") == 0) {
        g_node.fan_override = 255;
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        duty = 100;
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        duty = 0;
    } else if (argc < 2 || !parse_u32(argv[1], &duty) || duty > 100u) {
        printf("usage: fan on | off | <0-100> | auto\n");
        return 1;
    }
    g_node.fan_override = (uint8_t)duty; /* the reporter applies it and logs fan_state */
    return 0;
}

static int cmd_mark(int argc, char **argv)
{
    char text[96];
    size_t n = 0;
    for (int i = 1; i < argc && n < sizeof(text) - 1; i++) {
        if (i > 1) {
            text[n++] = ' ';
        }
        for (const char *c = argv[i]; *c && n < sizeof(text) - 1; c++) {
            if (*c >= 0x20 && *c < 0x7F && *c != '"' && *c != '\\') {
                text[n++] = *c;
            }
        }
    }
    text[n] = '\0';
    char body[160];
    snprintf(body, sizeof(body), "\"type\":\"mark\",\"text\":\"%s\"", text);
    node_echo_event(body);
    return 0;
}

static int cmd_ledger(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        node_ledger_print_status();
        return 0;
    }
    if (strcmp(sub, "tail") == 0) {
        uint32_t count = 10;
        if (argc >= 3) {
            parse_u32(argv[2], &count);
        }
        node_ledger_tail(count);
        return 0;
    }
    if (strcmp(sub, "check") == 0) {
        const bool sig = argc >= 3 && strcmp(argv[2], "sig") == 0;
        printf("checking%s...\n", sig ? " with signatures (slow)" : "");
        fx_chain_report_t rep;
        if (node_ledger_check(sig, &rep)) {
            printf("PASS  %" PRIu32 " entries\n", rep.entries);
            return 0;
        }
        printf("FAIL  at seq %" PRIu64 ": %s\n", rep.failed_seq, rep.reason ? rep.reason : "?");
        return 1;
    }
    if (strcmp(sub, "export") == 0) {
        uint32_t from = 0;
        if (argc >= 3) {
            parse_u32(argv[2], &from);
        }
        node_ledger_export(from);
        return 0;
    }
    if (strcmp(sub, "key") == 0) {
        char pem[256];
        if (fx_device_key_pem(&g_node.key, pem, sizeof(pem))) {
            printf("%s", pem);
            return 0;
        }
        printf("no device key\n");
        return 1;
    }
    if (strcmp(sub, "wipe") == 0) {
        if (argc < 3 || strcmp(argv[2], "ERASE-ALL-EVIDENCE") != 0) {
            printf("this deletes every ledger entry on the node. Type: ledger wipe ERASE-ALL-EVIDENCE\n");
            return 1;
        }
        printf("removed %d segment files; the next entry starts a new chain at seq 0\n", node_ledger_wipe());
        return 0;
    }
    printf("usage: ledger status | tail [n] | check [sig] | export [from_seq] | key | wipe ERASE-ALL-EVIDENCE\n");
    return 1;
}

static int cmd_media(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        node_media_list();
        return 0;
    }
#if CONFIG_FIREX_PRODUCTION
    printf("usage: media list   (a production image sends photos only to its gateway)\n");
#else
    if (argc >= 3 && strcmp(argv[1], "get") == 0) {
        node_media_get(argv[2]);
        return 0;
    }
    printf("usage: media list | get <name>\n");
#endif
    return 1;
}

static int cmd_selftest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!g_node.selftest_task) {
        return 1;
    }
#if CONFIG_FIREX_BENCH_INJECT
    portENTER_CRITICAL(&g_node.state_lock);
    const bool injecting = g_node.injecting;
    portEXIT_CRITICAL(&g_node.state_lock);
    if (injecting) {
        printf("self-test refused: a flame injection is running\n");
        return 1;
    }
#endif
    xTaskNotifyGive(g_node.selftest_task);
    printf("self-test requested\n");
    return 0;
}

#if CONFIG_FIREX_BENCH_INJECT
static int cmd_inject(int argc, char **argv)
{
    uint32_t seconds = 0;
    if (argc != 3 || strcmp(argv[1], "flame") != 0 || !parse_u32(argv[2], &seconds) || seconds < 3u || seconds > 30u) {
        printf("usage: inject flame <seconds 3-30>\n");
        return 1;
    }
    const char *why = NULL;
    if (!node_inject_flame(seconds, &why)) {
        printf("inject refused: %s\n", why ? why : "?");
        return 1;
    }
    printf("flame injection of %" PRIu32 " s requested\n", seconds);
    return 0;
}
#endif

static int cmd_cam(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "ping") == 0) {
        printf("camera %s\n", g_node.cam_ok && fx_cam_ping(&g_node.cam, 1500) ? "answered" : "did NOT answer");
        return 0;
    }
    printf("usage: cam ping\n");
    return 1;
}

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!g_node.i2c) {
        printf("I2C bus did not start\n");
        return 1;
    }
    int found = 0;
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(g_node.i2c, addr, 20) == ESP_OK) {
            printf("  0x%02x%s\n", addr, (addr >= 0x48 && addr <= 0x4B) ? "  (ADS1115 range)" : "");
            found++;
        }
    }
    printf("%d device(s)\n", found);
    return 0;
}

static int cmd_pairing(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!g_node.pairing_ok) {
        printf("no pairing code: NVS failed at boot\n");
        return 1;
    }
    char formatted[12];
    fx_pairing_code_format(g_node.pairing_code, formatted);
    printf("pairing code %s\n", formatted);
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        char line[160];
        fx_wifi_describe(line, sizeof(line));
        printf("%s\n", line);
        return 0;
    }
    if (strcmp(sub, "set") == 0 && (argc == 3 || argc == 4)) {
        const esp_err_t err = fx_wifi_set_credentials(argv[2], argc == 4 ? argv[3] : "");
        if (err != ESP_OK) {
            printf("could not set the network: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("joining \"%s\"; it is kept for later boots\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "forget") == 0) {
        const esp_err_t err = fx_wifi_forget();
        if (err != ESP_OK) {
            printf("could not erase the network: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("the stored network is erased; onboarding starts at the next boot\n");
        return 0;
    }
    printf("usage: wifi status | set <ssid> [password] | forget\n");
    return 1;
}

static int cmd_gateway(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "show";
    char host[FX_NET_HOST_MAX];
    uint16_t port = 0;
    if (strcmp(sub, "show") == 0) {
        if (fx_net_gateway_get(host, &port) == ESP_OK) {
            printf("%s port %u\n", host, (unsigned)port);
        } else {
            printf("no gateway set\n");
        }
        return 0;
    }
    if (strcmp(sub, "set") == 0 && argc == 3) {
        if (!fx_net_parse_host_port(argv[2], NODE_PROVISION_PORT, host, &port)) {
            printf("give a host name or IPv4 address, with an optional :port\n");
            return 1;
        }
        const esp_err_t err = fx_net_gateway_set(host, port);
        if (err != ESP_OK) {
            printf("could not store it: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("gateway %s port %u\n", host, (unsigned)port);
        node_net_request_provision();
        return 0;
    }
    printf("usage: gateway show | set <host[:port]>\n");
    return 1;
}

static int cmd_net(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        node_net_print_status();
        return 0;
    }
    if (strcmp(sub, "provision") == 0) {
        node_net_request_provision();
        printf("provisioning requested\n");
        return 0;
    }
    if (strcmp(sub, "forget") == 0) {
        const bool all = argc >= 3 && strcmp(argv[2], "all") == 0;
        node_net_request_forget(all);
        printf("forgetting the site credentials%s. To join a site again, add the unit on its dashboard.\n",
               all ? " and the gateway address" : "");
        return 0;
    }
    printf("usage: net status | provision | forget [all]\n");
    return 1;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        node_ota_print_status();
        return 0;
    }
    printf("usage: ota status   (updates come only from the site gateway, signed)\n");
    return 1;
}

static int cmd_clock(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    node_clock_print();
    return 0;
}

static int cmd_trace(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        node_trace_print_status();
        return 0;
    }
    printf("usage: trace status\n");
    return 1;
}

static void add(const char *name, const char *hint, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = {.command = name, .help = help, .hint = hint, .func = fn};
    if (esp_console_cmd_register(&cmd) != ESP_OK) {
        ESP_LOGE(TAG, "register %s", name);
    }
}

void node_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "firex>";
    repl_config.max_cmdline_length = 128;
    repl_config.task_priority = 2;
    repl_config.task_stack_size = 8192;

    esp_err_t err = ESP_FAIL;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t dev = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&dev, &repl_config, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&dev, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console did not start (%s); the alarm is unaffected", esp_err_to_name(err));
        return;
    }

    esp_console_register_help_command();
    add("status", NULL, "What is running, faults, ledger and camera", cmd_status);
    add("read", NULL, "Scores and raw values", cmd_read);
    add("stream", "on [period_ms] | off", "S lines for tools/capture", cmd_stream);
    add("baseline", "mq2|mq135 [samples] [tolerance] [force]", "Capture R0 in clean air", cmd_baseline);
    add("r0", "mq2|mq135 <ohms>", "Set R0 by hand", cmd_r0);
    add("config", "list | get <key> | set <key> <value> | reset <key>", "Settings in NVS", cmd_config);
    add("fan", "on | off | <0-100> | auto", "Bench override of the fan duty", cmd_fan);
    add("mark", "<text>", "Timestamped marker line", cmd_mark);
    add("ledger", "status | tail [n] | check [sig] | export [from] | key | wipe", "The on-device ledger", cmd_ledger);
#if CONFIG_FIREX_PRODUCTION
    add("media", "list", "Captured frames, by hash", cmd_media);
#else
    add("media", "list | get <name>", "Captured frames", cmd_media);
#endif
    add("selftest", NULL, "Run the self-test now", cmd_selftest);
    add("cam", "ping", "Camera link", cmd_cam);
    add("i2cscan", NULL, "List I2C addresses that answer", cmd_i2cscan);
    add("pairing", NULL, "This unit's pairing code", cmd_pairing);
    add("wifi", "status | set <ssid> [password] | forget", "The site Wi-Fi network", cmd_wifi);
    add("gateway", "show | set <host[:port]>", "The gateway to provision with", cmd_gateway);
    add("net", "status | provision | forget [all]", "The link to the site gateway", cmd_net);
    add("ota", "status", "Firmware slots, confirmation and the last update", cmd_ota);
    add("clock", NULL, "The ledger clock and its last gateway sync", cmd_clock);
    add("trace", "status", "Episode traces waiting for the gateway", cmd_trace);
    node_console_security_register();
#if CONFIG_FIREX_BENCH_INJECT
    add("inject", "flame <seconds>", "BENCH IMAGE: flicker the IR LED at the flame sensor", cmd_inject);
#endif

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console start: %s", esp_err_to_name(err));
    }
}
