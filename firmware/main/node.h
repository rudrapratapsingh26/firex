/*
 * The node's shared state and the task entry points.
 *
 * Task priorities, highest first, by what must still work when everything else
 * has failed (Software Architecture v1.0 section 04):
 *
 *   alarm     10   buzzer and ring from shared state; never waits on a queue
 *   fusion     8   sensors -> engine -> events and shared state, every 500 ms
 *   sensors  6-7   one task per sensor driver (from M1)
 *   ledger     5   drains the event queue: hash, sign, store
 *   camera     3   capture on the CRITICAL transition only
 *   reporter   3   serial stream for tools/capture and fan health
 *   selftest   2   daily checks
 *   console    2   the REPL
 *   net        2   Wi-Fi, provisioning, MQTT to the site gateway (M3)
 *
 * Alarm, fusion and sensor tasks are on the task watchdog.
 */
#ifndef FIREX_NODE_H
#define FIREX_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fx_ads1115.h"
#include "fx_alarm.h"
#include "fx_alarm_hw.h"
#include "fx_cam_link.h"
#include "fx_debounce.h"
#include "fx_dht22_driver.h"
#include "fx_events.h"
#include "fx_fan.h"
#include "fx_flame_driver.h"
#include "fx_ledger.h"
#include "fx_ledger_hw.h"
#include "fx_mq_driver.h"
#include "fx_net_hw.h"
#include "fx_ota.h"
#include "fx_selftest.h"
#include "fx_sensor.h"
#include "fx_settings.h"
#include "fx_sync.h"
#include "sdkconfig.h"
#if CONFIG_FIREX_HEAT_NTC
#include "fx_ntc_driver.h"
#endif

#if CONFIG_FIREX_HEAT_NTC
#define NODE_SENSOR_COUNT 5 /* mq2, mq135, ky026, ntc (heat), dht22 (humidity only) */
#else
#define NODE_SENSOR_COUNT 4 /* mq2, mq135, ky026, dht22 (heat and humidity) */
#endif
#define NODE_LEDGER_BASE "/ledger"
#define NODE_MEDIA_BASE "/media"
#define NODE_LEDGER_SEG_BYTES 32768u
#define NODE_LEDGER_MAX_SEGS 24u /* 768 KiB of the 1 MiB partition: room for LittleFS metadata */

#if CONFIG_FIREX_NET_ENABLE
#define NODE_PROVISION_PORT CONFIG_FIREX_GATEWAY_PROVISION_PORT
#else
#define NODE_PROVISION_PORT 8081
#endif

typedef struct {
    /* sensors and actuators */
    i2c_master_bus_handle_t i2c;
    fx_ads1115_t adc;
    bool adc_ok;
    fx_mq_driver_t mq2;
    fx_mq_driver_t mq135;
    fx_flame_driver_t flame;
    fx_dht22_driver_t dht;
#if CONFIG_FIREX_HEAT_NTC
    fx_ntc_driver_t ntc;
#endif
    fx_sensor_t *sensors[NODE_SENSOR_COUNT];
    fx_fan_t fan;
    bool fan_ok;
    uint8_t fan_override;   /* 0-100 set from the console, 255 = follow settings */
    bool fan_test;          /* the self-test holds the fan at full duty */
    fx_buzzer_t buzzer;
    fx_status_ring_t ring;
    fx_cam_link_t cam;
    bool cam_ok;

    /* settings */
    portMUX_TYPE settings_lock;
    fx_settings_t settings;
    uint32_t settings_version;

    /* identity and ledger */
    char fw_version[32];
    uint8_t fw_hash[32];
    uint32_t boot_count;
    const char *reset_reason;
    bool warm_start;            /* the reset kept the MQ heaters powered: fx_warm_s applies */
    fx_device_key_t key;
    bool key_ok;
    fx_tls_key_t tls_key;
    bool tls_ok;
    char pairing_code[11]; /* normalised; the label shows it as XXXXX-XXXXX */
    bool pairing_ok;
    fx_fs_store_t store;
    fx_ring_t ring_buffer;
    fx_ledger_t ledger;
    bool ledger_ok;
    SemaphoreHandle_t ledger_mutex;
    QueueHandle_t ledger_queue;
    fx_clock_t clock;
    bool media_ok;

    /* live state; the alarm task reads it under state_lock */
    portMUX_TYPE state_lock;
    bool have_fusion;
    fx_state_t fusion_state;
    bool sensor_fault;
    bool ledger_fault;
    bool selftest_fault;
    bool self_test_running;
    uint8_t selftest_mask;      /* FX_MOD_BIT of modalities excluded during a loopback */
    uint32_t chirp_until_ms;    /* self-test buzzer chirp */
    bool buzzer_error;
    fx_alarm_state_t alarm_state;
    fx_sample_t samples[FX_MODALITY_COUNT];
    float effective[FX_MODALITY_COUNT];
    uint32_t ledger_dropped;
    uint32_t inject_seconds;    /* bench image: a requested flame injection, 0 for none */
    bool injecting;             /* bench image: requested or flickering */
    uint32_t alarm_loops;       /* advanced by the alarm task, watched by a new image's confirm check */
    uint32_t fusion_loops;      /* advanced by the fusion task, likewise */

    /* tasks */
    TaskHandle_t camera_task;
    TaskHandle_t selftest_task;
    uint32_t captures;

    /* serial stream (M1 capture protocol) */
    portMUX_TYPE stream_lock;
    bool streaming;
    bool header_pending;
    uint32_t stream_period_ms;
    uint16_t baseline_samples;
} fx_node_t;

extern fx_node_t g_node;

uint32_t node_now_ms(void);

/* --- settings (settings_nvs.c) ---------------------------------------------- */
void node_settings_load(void);
fx_settings_t node_settings_snapshot(uint32_t *version);
/* Validate, persist, bump the version and (when log is set) write config_change. */
fx_set_status_t node_setting_change(const char *key, uint32_t value, bool log, const char **why);
uint32_t node_boot_count_next(void);

/* --- ledger (ledger_task.c) -------------------------------------------------- */
void node_ledger_start(void);
/* Queue an event without blocking. False (and a ledger fault) if the queue is full. */
bool node_log(const char *type, const uint8_t *payload, size_t len);
/* Serial-only event for the capture tool (marks): not a ledger entry. */
void node_echo_event(const char *json_body);
void node_ledger_print_status(void);
void node_ledger_tail(uint32_t count);
void node_ledger_export(uint64_t from_seq);
bool node_ledger_check(bool signatures, fx_chain_report_t *report);
bool node_ledger_verify_newest(void);
int node_ledger_wipe(void);

/* --- tasks ------------------------------------------------------------------ */
void node_fusion_start(void);
void node_alarm_start(void);
void node_camera_start(void);
void node_selftest_start(void);
#if CONFIG_FIREX_BENCH_INJECT
/* Bench image only: flicker the self-test IR LED at the flame sensor for
 * `seconds` (3-30), run by the self-test task. False, with a reason, if refused. */
bool node_inject_flame(uint32_t seconds, const char **why);
#endif
void node_reporter_start(void);
void node_stream(bool on, uint32_t period_ms);
void node_print_snapshot(void);
void node_console_start(void);
/* `se` and `security` (console_security.c, M6): the secure element and the eFuse state. */
void node_console_security_register(void);
/* Episode traces on the media partition, for the gateway (trace_task.c, M5). */
void node_trace_start(void);
void node_trace_print_status(void);
/* Gateway time for the ledger clock (ledger_task.c, M5): writes clock_sync on the first
 * sync of a boot or a step of FX_CLOCK_LOG_STEP_MS or more. */
void node_clock_sync(uint64_t unix_ms);
void node_clock_print(void);

/* --- network (net_task.c) ------------------------------------------------------ */
/* The TLS key and the pairing code. Before any ADC driver, like the device key. */
void node_net_identity(void);
void node_net_start(void);
void node_net_print_status(void);
void node_net_request_provision(void);
void node_net_request_forget(bool gateway);
/* The highest seq the gateway holds on this chain; -1 for none, or no gateway. */
int64_t node_net_acked(void);
/* The site CA and this unit's certificate from provisioning, for the firmware download; NULL until provisioned. */
const fx_net_creds_t *node_net_creds(void);
/* Connected to the gateway, and a sync has arrived on this connection (M5). */
bool node_net_synced(void);

/* --- over-the-air updates (ota_task.c) ------------------------------------------ */
/* devkit, evt or evt-production, at FX_OTA_KIND_OFFSET in this image (image_kind.c, M6). */
extern const fx_ota_image_kind_t fx_image_kind;
/* After the sensors are up: a new image starts confirming itself, the old one after a
 * rollback records it, and the OTA task starts. */
void node_ota_start(void);
/* An `ota` command from the gateway. False, with a reason, if refused; the refusal is a
 * fw_update entry either way. */
bool node_ota_request(const fx_cmd_t *cmd, const char **why);
/* The running slot ("ota_0", "ota_1") and whether it has yet to confirm itself, for hello. */
void node_ota_describe(char *partition, size_t cap, bool *pending_verify);
void node_ota_print_status(void);

/* media helpers (camera_task.c) */
void node_media_list(void);
#if !CONFIG_FIREX_PRODUCTION
/* Serial export of one photo, for tools/ledger/export_log.py. Not in production images. */
void node_media_get(const char *name);
#endif

#endif /* FIREX_NODE_H */
