/*
 * Console commands for the secure element and the eFuse state (M6). Every line starts with
 * SE, or SEC, so tools/factory/provision.py can read it off the serial port.
 *
 *   se status                      the chip, its configuration zone and the next step
 *   se provision FIREX-EVT-LOCK    configure, lock and generate the ledger key; irreversible
 *   security status                secure boot, flash encryption, debug fuses, key digests
 *
 * `se status` prints
 *   SE,backend,<atecc608b|nvs>
 *   SE,present,<yes|no>
 *   SE,serial,<18 hex>  SE,revision,<8 hex>  SE,config,<256 hex>
 *   SE,step,<write_config|lock_config|lock_data|genkey_ledger|done|refuse>
 *   SE,why,<text>             (refuse only)
 *   SE,ledger_pub,<130 hex>   (done only)
 *
 * `se provision` prints SE,step,<name> as each step completes, then SE,ledger_pub and
 * SE,result,ok and restarts, so the ledger opens with the new key. An already provisioned
 * chip gets SE,result,ok,already and no restart; a failure SE,result,fail,<reason>.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fx_entry.h"
#include "fx_secure.h"
#include "fx_security.h"
#include "node.h"
#include "sdkconfig.h"

#define PROVISION_TOKEN "FIREX-EVT-LOCK"

static const char *TAG = "console_security";

#if CONFIG_FIREX_KEY_ATECC608B

static void print_hex(const char *prefix, const uint8_t *data, size_t len)
{
    char hex[2u * FX_SE_CONFIG_SIZE + 1u];
    if (len > FX_SE_CONFIG_SIZE) {
        len = FX_SE_CONFIG_SIZE;
    }
    fx_hex_encode(data, len, hex);
    printf("%s,%s\n", prefix, hex);
}

static fx_se_t *secure_element(void)
{
    fx_se_t *se = &g_node.key.se;
    if (!se->open && fx_se_open(se) != ESP_OK) {
        return NULL;
    }
    return se;
}

static void print_ledger_pub(fx_se_t *se)
{
    uint8_t pub[65];
    if (fx_se_public_key(se, FX_SE_SLOT_LEDGER, pub) == ESP_OK) {
        print_hex("SE,ledger_pub", pub, sizeof(pub));
    }
}

static void on_step(fx_se_step_t done, void *ctx)
{
    (void)ctx;
    printf("SE,step,%s\n", fx_se_step_name(done));
    fflush(stdout);
}

#endif /* CONFIG_FIREX_KEY_ATECC608B */

static int se_status(void)
{
#if CONFIG_FIREX_KEY_ATECC608B
    printf("SE,backend,atecc608b\n");
    fx_se_t *se = secure_element();
    if (!se) {
        printf("SE,present,no\n");
        return 1;
    }
    printf("SE,present,yes\n");
    print_hex("SE,serial", se->serial, sizeof(se->serial));
    print_hex("SE,revision", se->revision, sizeof(se->revision));
    const char *why = NULL;
    const fx_se_step_t step = fx_se_state(se, &why);
    print_hex("SE,config", se->config, sizeof(se->config));
    printf("SE,step,%s\n", fx_se_step_name(step));
    if (step == FX_SE_STEP_REFUSE) {
        printf("SE,why,%s\n", why ? why : "?");
    } else if (step == FX_SE_STEP_DONE) {
        print_ledger_pub(se);
    }
    return 0;
#else
    printf("SE,backend,nvs\n");
    printf("SE,present,no\n");
    return 0;
#endif
}

static int se_provision(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[2], PROVISION_TOKEN) != 0) {
        printf("this configures and locks the secure element for life and generates the unit's ledger key; it "
               "cannot be undone.\nusage: se provision " PROVISION_TOKEN "\n");
        return 1;
    }
#if CONFIG_FIREX_KEY_ATECC608B
    fx_se_t *se = secure_element();
    if (!se) {
        printf("SE,result,fail,no secure element answered\n");
        return 1;
    }
    const char *why = NULL;
    fx_se_step_t step = fx_se_state(se, &why);
    if (step == FX_SE_STEP_DONE) {
        print_ledger_pub(se);
        printf("SE,result,ok,already\n");
        return 0;
    }
    if (step == FX_SE_STEP_REFUSE) {
        printf("SE,result,fail,%s\n", why ? why : "refused");
        return 1;
    }
    ESP_LOGW(TAG, "provisioning the secure element from step %s", fx_se_step_name(step));
    uint8_t pub[65];
    if (fx_se_provision(se, on_step, NULL, pub, &why) != ESP_OK) {
        printf("SE,result,fail,%s\n", why ? why : "?");
        return 1;
    }
    print_hex("SE,ledger_pub", pub, sizeof(pub));
    printf("SE,result,ok\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return 0;
#else
    printf("SE,result,fail,this image has no secure element\n");
    return 1;
#endif
}

static int cmd_se(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        return se_status();
    }
    if (strcmp(sub, "provision") == 0) {
        return se_provision(argc, argv);
    }
    printf("usage: se status | provision " PROVISION_TOKEN "\n");
    return 1;
}

static int cmd_security(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "status") != 0) {
        printf("usage: security status\n");
        return 1;
    }
    fx_security_status_t st;
    fx_security_status_read(&st);
    printf("SEC,secure_boot,%s\n", st.secure_boot ? "on" : "off");
    printf("SEC,flash_encryption,%s\n", st.flash_encryption);
    printf("SEC,jtag,%s\n", st.pad_jtag_disabled ? "disabled" : "enabled");
    printf("SEC,usb_jtag,%s\n", st.usb_jtag_disabled ? "disabled" : "enabled");
    printf("SEC,rom_download,%s\n", st.rom_download);
    for (int i = 0; i < 3; i++) {
        if (st.digest_state[i] == FX_DIGEST_PRESENT) {
            char hex[65];
            fx_hex_encode(st.digest[i], 32, hex);
            printf("SEC,key_digest,%d,%s\n", i, hex);
        } else {
            printf("SEC,key_digest,%d,%s\n", i, st.digest_state[i] == FX_DIGEST_REVOKED ? "revoked" : "none");
        }
    }
    printf("SEC,nvs_encryption,%s\n", st.nvs_encryption ? "on" : "off");
#if CONFIG_FIREX_KEY_ATECC608B
    printf("SEC,device_key,atecc608b\n");
#else
    printf("SEC,device_key,nvs\n");
#endif
#if CONFIG_FIREX_PRODUCTION
    printf("SEC,production,yes\n");
#else
    printf("SEC,production,no\n");
#endif
    char fw_hash[65];
    fx_hex_encode(g_node.fw_hash, sizeof(g_node.fw_hash), fw_hash);
    printf("SEC,firmware,%s,%s\n", g_node.fw_version, fw_hash);
    return 0;
}

void node_console_security_register(void)
{
    const esp_console_cmd_t se = {
        .command = "se",
        .help = "The secure element: its state, and factory provisioning (irreversible)",
        .hint = "status | provision " PROVISION_TOKEN,
        .func = cmd_se,
    };
    const esp_console_cmd_t security = {
        .command = "security",
        .help = "Secure boot, flash encryption, debug interfaces and key digests",
        .hint = "status",
        .func = cmd_security,
    };
    if (esp_console_cmd_register(&se) != ESP_OK || esp_console_cmd_register(&security) != ESP_OK) {
        ESP_LOGE(TAG, "register se/security");
    }
}
