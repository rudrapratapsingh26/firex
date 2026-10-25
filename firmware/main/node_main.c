/*
 * FireX node firmware, M3.
 *
 * Start-up order matters:
 *   1. NVS, settings, boot counter, firmware identity
 *   2. the device key, the TLS key and the pairing code, before any ADC driver
 *      (generating them uses the SAR ADC as an entropy source)
 *   3. ledger queue and task, ledger partition, chain recovery, media partition
 *   4. boot entry
 *   5. alarm hardware and the alarm task, so the ring shows BOOT at once
 *   6. sensors and their tasks, fusion, camera, self-test, reporter
 *   7. network: Wi-Fi, provisioning, MQTT to the gateway
 *   8. console
 *
 * A failure in any step after 5 leaves the alarm running on whatever sensors
 * came up, and shows FAULT.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "node.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef FX_BUILD_EPOCH
#define FX_BUILD_EPOCH 1789000000ULL
#endif

#if CONFIG_FIREX_PRODUCTION
/* A production image means nothing without its locks: fail the build, not the unit. */
#if !CONFIG_SECURE_BOOT_V2_ENABLED || !CONFIG_SECURE_FLASH_ENC_ENABLED || !CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
#error "CONFIG_FIREX_PRODUCTION needs secure boot v2 and flash encryption in release mode (sdkconfig.production)"
#endif
#if !CONFIG_FIREX_KEY_ATECC608B
#error "CONFIG_FIREX_PRODUCTION needs the ATECC608B device key"
#endif
#if CONFIG_FIREX_BENCH_INJECT
#error "a production image cannot carry the bench injection"
#endif
#if CONFIG_SECURE_BOOT_ALLOW_JTAG || CONFIG_SECURE_INSECURE_ALLOW_DL_MODE || !CONFIG_NVS_ENCRYPTION
#error "a production image keeps JTAG off, the ROM loader restricted and NVS encrypted"
#endif
#endif

static const char *TAG = "firex";

fx_node_t g_node;

uint32_t node_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Erasing NVS also erases the device key; the ledger will then refuse to
         * extend the old chain and show FAULT until it is exported and wiped. */
        ESP_LOGE(TAG, "NVS unusable (%s): erasing it", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
}

static void identify(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
#if CONFIG_FIREX_BENCH_INJECT
    /* A bench image says so wherever its version appears: banner, boot entry, gateway hello. */
    snprintf(g_node.fw_version, sizeof(g_node.fw_version), "%.24s+inject", app->version);
#else
    snprintf(g_node.fw_version, sizeof(g_node.fw_version), "%s", app->version);
#endif
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || esp_partition_get_sha256(running, g_node.fw_hash) != ESP_OK) {
        ESP_LOGE(TAG, "could not hash the running image");
    }
    g_node.boot_count = node_boot_count_next();
    g_node.reset_reason = reset_reason_name(esp_reset_reason());
    g_node.warm_start = fx_ota_heaters_kept_power(g_node.reset_reason);
}

static void set_ledger_fault(bool fault)
{
    portENTER_CRITICAL(&g_node.state_lock);
    g_node.ledger_fault = fault;
    portEXIT_CRITICAL(&g_node.state_lock);
}

static void open_ledger(void)
{
    bool formatted = false;
    if (fx_fs_mount("ledger", NODE_LEDGER_BASE, &formatted) != ESP_OK) {
        ESP_LOGE(TAG, "ledger partition did not mount");
        set_ledger_fault(true);
        return;
    }
    if (formatted) {
        ESP_LOGW(TAG, "ledger partition was blank and has been formatted");
    }
    fx_store_ops_t ops;
    fx_fs_store_init(&g_node.store, NODE_LEDGER_BASE, &ops);
    xSemaphoreTake(g_node.ledger_mutex, portMAX_DELAY);
    const int rc = fx_ring_open(&g_node.ring_buffer, &ops, NODE_LEDGER_SEG_BYTES, NODE_LEDGER_MAX_SEGS);
    fx_clock_init(&g_node.clock, g_node.ring_buffer.have_last, g_node.ring_buffer.last_ts, FX_BUILD_EPOCH);
    fx_ledger_status_t st = FX_LEDGER_BLOCKED;
    if (rc == 0 && g_node.key_ok) {
        st = fx_ledger_init(&g_node.ledger, &g_node.ring_buffer, fx_device_key_signer(&g_node.key),
                            g_node.key.device_id, g_node.fw_hash);
    }
    g_node.ledger_ok = rc == 0 && g_node.key_ok && st == FX_LEDGER_OK;
    xSemaphoreGive(g_node.ledger_mutex);

    if (st == FX_LEDGER_BLOCKED && g_node.key_ok && rc == 0) {
        ESP_LOGE(TAG, "the log on flash was written under a different device key: export it, then `ledger wipe`");
    }
    set_ledger_fault(!g_node.ledger_ok);
}

static void setup_sensors(const fx_settings_t *s)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_FIREX_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_FIREX_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &g_node.i2c) == ESP_OK) {
        g_node.adc_ok = fx_ads1115_open(g_node.i2c, CONFIG_FIREX_ADS1115_ADDR, FX_ADS_PGA_4V096, FX_ADS_DR_128,
                                        &g_node.adc) == ESP_OK;
    } else {
        g_node.i2c = NULL;
    }

    const float top = (float)CONFIG_FIREX_AO_DIVIDER_TOP_OHMS, bottom = (float)CONFIG_FIREX_AO_DIVIDER_BOTTOM_OHMS;
    const float divider = bottom > 0.0f ? bottom / (top + bottom) : 1.0f;
    fx_mq_params_t mq2, mq135;
    fx_mq2_defaults(&mq2);
    fx_mq135_defaults(&mq135);
    mq2.r_load = (float)CONFIG_FIREX_MQ2_RL_OHMS;
    mq135.r_load = (float)CONFIG_FIREX_MQ135_RL_OHMS;
    mq2.divider = mq135.divider = divider;
    fx_settings_mq(s, &mq2, &mq135);

    fx_ads1115_t *adc = g_node.adc_ok ? &g_node.adc : NULL;
    fx_mq_driver_setup(&g_node.mq2, "mq2", FX_GAS, adc, CONFIG_FIREX_MQ2_CHANNEL, &mq2, fx_settings_r0(s, FX_GAS));
    fx_mq_driver_setup(&g_node.mq135, "mq135", FX_SMOKE, adc, CONFIG_FIREX_MQ135_CHANNEL, &mq135,
                       fx_settings_r0(s, FX_SMOKE));

    fx_flame_params_t flame;
    fx_settings_flame(s, &flame);
    fx_flame_driver_setup(&g_node.flame, CONFIG_FIREX_FLAME_GPIO, &flame);
    fx_heat_params_t heat;
    fx_settings_heat(s, &heat);
    fx_dht22_driver_setup(&g_node.dht, CONFIG_FIREX_DHT22_GPIO, &heat);

    g_node.sensors[0] = &g_node.mq2.sensor;
    g_node.sensors[1] = &g_node.mq135.sensor;
    g_node.sensors[2] = &g_node.flame.sensor;
#if CONFIG_FIREX_HEAT_NTC
    /* EVT board: the thermistor owns heat, and the DHT22 stays for humidity. */
    fx_ntc_params_t ntc;
    fx_ntc_defaults(&ntc);
    ntc.fixed_ohms = (float)CONFIG_FIREX_NTC_FIXED_OHMS;
    ntc.r25_ohms = (float)CONFIG_FIREX_NTC_R25_OHMS;
    ntc.beta = (float)CONFIG_FIREX_NTC_BETA;
    ntc.supply_v = (float)CONFIG_FIREX_NTC_SUPPLY_MV / 1000.0f;
#if CONFIG_FIREX_NTC_CHANNEL == CONFIG_FIREX_MQ2_CHANNEL || CONFIG_FIREX_NTC_CHANNEL == CONFIG_FIREX_MQ135_CHANNEL
    /* Reading a gas sensor's input as a temperature would be worse than no heat at all. */
    ESP_LOGE(TAG, "thermistor on ADS1115 AIN%d, which is an MQ input: heat stays invalid", CONFIG_FIREX_NTC_CHANNEL);
    fx_ads1115_t *const ntc_adc = NULL;
#else
    fx_ads1115_t *const ntc_adc = adc;
#endif
    fx_ntc_driver_setup(&g_node.ntc, ntc_adc, CONFIG_FIREX_NTC_CHANNEL, CONFIG_FIREX_NTC_PERIOD_MS, &ntc, &heat);
    fx_dht22_driver_humidity_only(&g_node.dht);
    g_node.sensors[3] = &g_node.ntc.sensor;
    g_node.sensors[4] = &g_node.dht.sensor;
#else
    g_node.sensors[3] = &g_node.dht.sensor;
#endif

    g_node.fan_ok = fx_fan_init(&g_node.fan, CONFIG_FIREX_FAN_PWM_GPIO, CONFIG_FIREX_FAN_TACH_GPIO, node_now_ms()) ==
                    ESP_OK;
}

#include "esp_task_wdt.h"

static void sensor_task(void *arg)
{
    fx_sensor_t *s = (fx_sensor_t *)arg;
    TickType_t period = pdMS_TO_TICKS(s->period_ms);
    if (period == 0) {
        period = 1;
    }
    esp_task_wdt_add(NULL);
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        s->poll(s, node_now_ms());
        esp_task_wdt_reset();
        xTaskDelayUntil(&wake, period);
    }
}

void app_main(void)
{
    memset(&g_node, 0, sizeof(g_node));
    portMUX_INITIALIZE(&g_node.settings_lock);
    portMUX_INITIALIZE(&g_node.state_lock);
    portMUX_INITIALIZE(&g_node.stream_lock);
    g_node.fan_override = 255;
    g_node.stream_period_ms = CONFIG_FIREX_STREAM_PERIOD_MS;

    init_nvs();
    node_settings_load();
    identify();
    printf("\nFireX node %s, boot %" PRIu32 " (%s)\n", g_node.fw_version, g_node.boot_count, g_node.reset_reason);

    g_node.key_ok = fx_device_key_open(&g_node.key) == ESP_OK;
    if (g_node.key.created) {
        ESP_LOGW(TAG, "a new device key was generated on this boot");
    }
    node_net_identity();

    node_ledger_start();
    open_ledger();
    g_node.media_ok = fx_fs_mount("media", NODE_MEDIA_BASE, NULL) == ESP_OK;

    uint8_t payload[FX_PAYLOAD_MAX];
    node_log("boot", payload, fx_ev_boot(payload, g_node.fw_version, g_node.reset_reason, g_node.boot_count));

    const fx_settings_t s = node_settings_snapshot(NULL);

#ifdef CONFIG_FIREX_BUZZER_PASSIVE
    fx_buzzer_init(&g_node.buzzer, CONFIG_FIREX_BUZZER_GPIO, FX_BUZZER_PASSIVE, CONFIG_FIREX_BUZZER_TONE_HZ);
#else
    fx_buzzer_init(&g_node.buzzer, CONFIG_FIREX_BUZZER_GPIO, FX_BUZZER_ACTIVE, 0);
#endif
    if (CONFIG_FIREX_RING_GPIO >= 0) {
        fx_status_ring_init(&g_node.ring, CONFIG_FIREX_RING_GPIO, CONFIG_FIREX_RING_LEDS);
    }
    node_alarm_start();

    setup_sensors(&s);
#if CONFIG_FIREX_HEAT_NTC
    static const UBaseType_t priority[NODE_SENSOR_COUNT] = {6, 6, 7, 6, 6};
#else
    static const UBaseType_t priority[NODE_SENSOR_COUNT] = {6, 6, 7, 6};
#endif
    for (int i = 0; i < NODE_SENSOR_COUNT; i++) {
        fx_sensor_t *sensor = g_node.sensors[i];
        sensor->ready = sensor->init(sensor) == 0;
        if (sensor->ready) {
            xTaskCreate(sensor_task, sensor->name, 4096, sensor, priority[i], NULL);
        }
    }

    g_node.cam_ok = fx_cam_link_init(&g_node.cam, 1, CONFIG_FIREX_CAM_UART_TX_GPIO, CONFIG_FIREX_CAM_UART_RX_GPIO,
                                     CONFIG_FIREX_CAM_BAUD) == ESP_OK;

    node_fusion_start();
    node_camera_start();
    node_trace_start();
    node_selftest_start();
    node_reporter_start();
    node_net_start();
    node_ota_start(); /* after the sensors: a new image compares them with the image before */
    node_console_start();
}
