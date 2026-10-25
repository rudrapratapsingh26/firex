/*
 * The ATECC608B configuration FireX writes, and the provisioning step that follows from a
 * configuration zone read off the chip (M6). Pure C, host-tested by firmware/Makefile.
 *
 * The chip is a blank ATECC608B (TrustCUSTOM). FireX changes eight bytes of its
 * configuration zone and keeps every other factory byte:
 *
 *   SlotConfig[0], SlotConfig[1] = 0x2081   external-message signatures only, IsSecret,
 *                                           WriteConfig 2: GenKey may make a random key,
 *                                           PrivWrite never
 *   KeyConfig[0],  KeyConfig[1]  = 0x0033   Private, PubInfo, KeyType P-256, Lockable
 *
 * Slot 0 holds the ledger's device key. Slot 1 is configured the same way for a later TLS
 * key and is not generated in M6: the configuration zone is locked for life, so the room is
 * made now.
 *
 * Provisioning, in order: write the configuration, lock it with its CRC, lock the data zone,
 * generate the key in slot 0, lock slot 0. A key counts as established only once its slot is
 * locked. If power fails between GenKey and the slot lock, the next run generates again: the
 * unlocked key was never used or reported.
 */
#ifndef FX_SE_CONFIG_H
#define FX_SE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_SE_CONFIG_SIZE 128u
#define FX_SE_SLOT_LEDGER 0u
#define FX_SE_SLOT_TLS 1u

/* Configuration zone layout (ATECC608, cryptoauthlib's atecc608_config_t). */
#define FX_SE_SLOT_CONFIG_AT(slot) (20u + 2u * (unsigned)(slot)) /* uint16, little-endian */
#define FX_SE_LOCK_VALUE_AT 86u                                  /* data and OTP zones */
#define FX_SE_LOCK_CONFIG_AT 87u                                 /* configuration zone */
#define FX_SE_SLOT_LOCKED_AT 88u                                 /* uint16 LE; bit n clear = slot n locked */
#define FX_SE_KEY_CONFIG_AT(slot) (96u + 2u * (unsigned)(slot))  /* uint16, little-endian */
#define FX_SE_UNLOCKED 0x55u                                     /* LockValue and LockConfig before locking */

#define FX_SE_SLOT_CONFIG 0x2081u
#define FX_SE_KEY_CONFIG 0x0033u

typedef struct {
    bool config_locked;      /* LockConfig is not 0x55 */
    bool data_locked;        /* LockValue is not 0x55 */
    bool ledger_slot_locked; /* SlotLocked bit 0 clear */
    bool tls_slot_locked;    /* SlotLocked bit 1 clear */
    bool slots_as_required;  /* slots 0 and 1 carry FireX's SlotConfig and KeyConfig */
} fx_se_config_view_t;

typedef enum {
    FX_SE_STEP_WRITE_CONFIG = 0,
    FX_SE_STEP_LOCK_CONFIG,
    FX_SE_STEP_LOCK_DATA,
    FX_SE_STEP_GENKEY_LEDGER,
    FX_SE_STEP_LOCK_LEDGER_SLOT, /* follows GenKey within one run; never read off a chip */
    FX_SE_STEP_DONE,
    FX_SE_STEP_REFUSE
} fx_se_step_t;

/* cryptoauthlib's atCRC: polynomial 0x8005, bits taken least significant first, initial 0.
 * The value is crc_le[0] | crc_le[1] << 8, the order the Lock command takes it in. */
uint16_t fx_se_crc16(const uint8_t *data, size_t len);

/* Write FireX's SlotConfig and KeyConfig for slots 0 and 1 into a configuration read off the
 * chip. No other byte changes. */
void fx_se_config_apply(uint8_t cfg[FX_SE_CONFIG_SIZE]);

void fx_se_config_view(const uint8_t cfg[FX_SE_CONFIG_SIZE], fx_se_config_view_t *out);

/* The next provisioning step for a chip with this configuration zone. Never returns
 * FX_SE_STEP_LOCK_LEDGER_SLOT: an unlocked slot 0 reads as "generate", whether or not a key
 * was made before. On FX_SE_STEP_REFUSE `why` names the reason. */
fx_se_step_t fx_se_next_step(const uint8_t cfg[FX_SE_CONFIG_SIZE], const char **why);

/* write_config, lock_config, lock_data, genkey_ledger, lock_ledger_slot, done, refuse. */
const char *fx_se_step_name(fx_se_step_t step);

#ifdef __cplusplus
}
#endif
#endif /* FX_SE_CONFIG_H */
