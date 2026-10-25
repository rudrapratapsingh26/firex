/*
 * What the chip's eFuses and the image say about the unit's protection (M6): secure boot,
 * flash encryption, the debug interfaces, the ROM loader, the secure boot key digests and
 * NVS encryption. Read-only; nothing here burns an eFuse.
 */
#ifndef FX_SECURITY_H
#define FX_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_DIGEST_NONE = 0, /* no key block carries this digest purpose */
    FX_DIGEST_PRESENT,
    FX_DIGEST_REVOKED
} fx_digest_state_t;

typedef struct {
    bool secure_boot;
    const char *flash_encryption; /* "release", "development" or "off" */
    bool pad_jtag_disabled;
    bool usb_jtag_disabled;
    const char *rom_download;     /* "disabled", "secure" or "open" */
    fx_digest_state_t digest_state[3];
    uint8_t digest[3][32];        /* valid when FX_DIGEST_PRESENT */
    bool nvs_encryption;          /* CONFIG_NVS_ENCRYPTION, and its HMAC key is in eFuse */
} fx_security_status_t;

void fx_security_status_read(fx_security_status_t *out);

#ifdef __cplusplus
}
#endif
#endif /* FX_SECURITY_H */
