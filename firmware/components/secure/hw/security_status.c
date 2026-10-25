#include "fx_security.h"

#include <string.h>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "sdkconfig.h"

static const esp_efuse_purpose_t DIGEST_PURPOSE[3] = {
    ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST0,
    ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST1,
    ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST2,
};

void fx_security_status_read(fx_security_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->secure_boot = esp_secure_boot_enabled();

    switch (esp_get_flash_encryption_mode()) {
    case ESP_FLASH_ENC_MODE_RELEASE: out->flash_encryption = "release"; break;
    case ESP_FLASH_ENC_MODE_DEVELOPMENT: out->flash_encryption = "development"; break;
    default: out->flash_encryption = "off"; break;
    }

    out->pad_jtag_disabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_PAD_JTAG);
    out->usb_jtag_disabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG);
    if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE)) {
        out->rom_download = "disabled";
    } else if (esp_efuse_read_field_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD)) {
        out->rom_download = "secure";
    } else {
        out->rom_download = "open";
    }

    static const esp_efuse_desc_t **const REVOKE[3] = {
        ESP_EFUSE_SECURE_BOOT_KEY_REVOKE0,
        ESP_EFUSE_SECURE_BOOT_KEY_REVOKE1,
        ESP_EFUSE_SECURE_BOOT_KEY_REVOKE2,
    };
    for (int i = 0; i < 3; i++) {
        esp_efuse_block_t block = EFUSE_BLK_MAX;
        if (esp_efuse_read_field_bit(REVOKE[i])) {
            out->digest_state[i] = FX_DIGEST_REVOKED;
        } else if (esp_efuse_find_purpose(DIGEST_PURPOSE[i], &block) &&
                   esp_efuse_read_block(block, out->digest[i], 0, 256) == ESP_OK) {
            out->digest_state[i] = FX_DIGEST_PRESENT;
        }
    }

#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
    if (CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID >= 0) {
        const esp_efuse_block_t block = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
        out->nvs_encryption = esp_efuse_get_key_purpose(block) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
    }
#elif CONFIG_NVS_ENCRYPTION
    out->nvs_encryption = esp_flash_encryption_enabled();
#endif
}
