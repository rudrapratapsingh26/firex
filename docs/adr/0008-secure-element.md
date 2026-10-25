# ADR 0008 — The device key in an ATECC608B

Date: 2026-09-16
Status: accepted for M6

## Context

Through M5 a unit's device key, the P-256 key that signs every ledger entry, is generated on
first boot and kept in plain NVS. Anyone with the board and a USB cable can read it and sign
entries as that unit. ADR 0004 accepted that for the bench and the pilots and named the ATECC608B
on the M6 PCB as the fix. The roadmap asks for "the device key generated in and never leaving the
secure element".

The EVT board has no schematic yet. ESP-IDF 5.5 and Espressif's `esp-cryptoauthlib` component
(cryptoauthlib 3.7.9) are the software available. The unit also holds a separate P-256 TLS key
for MQTT and firmware downloads (ADR 0005).

## Decisions

**A blank ATECC608B, configured and locked by FireX.** The part is the unprovisioned
(TrustCUSTOM) ATECC608B. FireX writes its configuration zone and locks it at the factory
(`tools/factory`, through the unit's console). Every configuration byte FireX does not set keeps
the chip's factory value.

| Config bytes | Field | Value (little-endian) | Meaning |
|--------------|-------|-----------------------|---------|
| 20-21 | SlotConfig[0] | 0x2081 (`81 20`) | Signs external messages; secret, never readable; GenKey allowed; no PrivWrite |
| 22-23 | SlotConfig[1] | 0x2081 | The same, reserved for the TLS key |
| 96-97 | KeyConfig[0] | 0x0033 (`33 00`) | Private; public key derivable; P-256; slot lockable |
| 98-99 | KeyConfig[1] | 0x0033 | The same |

The configuration zone is locked with the CRC of the bytes FireX expects, so a chip holding
anything else refuses the lock.

**Slot 0 holds the device key, generated in the chip.** Provisioning runs these steps in order:

1. Write the configuration.
2. Lock the configuration zone with its CRC.
3. Lock the data zone.
4. GenKey slot 0.
5. Lock slot 0.
6. Report the public key, then restart.

The private key is created inside the chip and never exists anywhere else.

**A key counts only once its slot is locked.** If power fails between GenKey and the slot lock,
the next run generates slot 0 again. Nothing has used an unlocked key, so nothing is lost. Once
locked, the key can never be replaced: a stored key that must not change becomes one that
cannot. At boot the firmware uses the chip only if:
- the configuration zone is locked with FireX's slot settings;
- the data zone is locked;
- slot 0 is locked.

Otherwise the device key does not open, the ledger stays blocked and the unit shows FAULT, as a
missing key does today. `se status` says which step is missing.

**A chip configured by someone else is refused.** A configuration zone locked with different
slot settings, such as a Trust&GO or TrustFLEX part fitted by mistake, is never used and never
provisioned.

**Slot 1 is configured now and not used.** The configuration zone can be written only once. Slot
1 gets the same settings as slot 0 so the TLS key can move into the chip later without a new part.
M6 does not generate it.

**Its own I2C bus.** cryptoauthlib's ESP32 HAL creates its own I2C bus from
`CONFIG_ATCA_I2C_SDA_PIN` and `CONFIG_ATCA_I2C_SCL_PIN`, so the chip cannot share the ADS1115's
bus. It sits on I2C controller 1 (`CONFIG_FIREX_ATECC_I2C_PORT`). The EVT placeholder pins are
GPIO 21 and 47 (`sdkconfig.evt`), until the schematic sets them.

**One mutex around the chip.** Only `components/secure` calls cryptoauthlib, and every call
holds one lock. Ledger signing, provisioning and `se status` never interleave on the wire.

**The signer interface does not change.** `fx_device_key_open`, `fx_device_key_signer` and
`fx_device_key_pem` keep their signatures. `CONFIG_FIREX_KEY_ATECC608B` selects the secure
element backend. The chain writer, provisioning and the console see the same key API on both
boards, and the device ID is still derived from the public key.

## Rejected

- **The secure element for the TLS key through esp-tls.** ESP-IDF's esp-tls can take a TLS
  client key from an ATECC608A, but its implementation calls `atcab_init` on every connection
  and `atcab_release` when a connection closes. That would tear down the chip session the ledger
  task signs with, at any reconnect. Its default cryptoauthlib interface configuration also names
  a fixed I2C bus. A custom mbedTLS key backed by our mutex is the way to move the TLS key later,
  into slot 1.
- **TrustFLEX.** A pre-configured part with a slot 0 key made at Microchip. It avoids the
  factory's one-shot configuration lock, but the key is not generated in the unit's own life, the
  data slots stay regenerable, and the part depends on Microchip's provisioning manifest.
- **Keeping the device key in NVS behind flash encryption only.** On a production unit that would
  keep the key off a flash dump, but the key would still exist in the ESP32-S3's memory and could
  be recovered by anything that runs code on it. The secure element makes the key's absence from
  the main MCU a property of the hardware.

## Consequences

- An EVT unit's device key cannot be copied. The threat model's "extracted dev-kit key" limit is
  closed for EVT units and stays open for dev-kit and pilot units, which keep the NVS key.
- The TLS key stays in NVS. On a production unit it is protected by NVS encryption, flash
  encryption and secure boot (ADR 0009), not by hardware. A stolen TLS key lets someone connect
  to the broker as the unit but not write ledger entries the gateway accepts. Removal revokes the
  certificate.
- Signatures from the chip are randomised: its own random number generator makes each nonce.
  Dev-kit signatures stay deterministic (RFC 6979). Both verify identically, and the verifier
  needs no change (SPEC §7).
- A signature takes the chip longer than a software signature on the S3 (not measured). Entries
  are signed by the ledger task, which producers never wait on.
- Provisioning is irreversible and is done only through the factory tool. A chip locked wrongly
  is scrap. Bench rehearsal uses spare chips.
- A unit whose secure element fails has no device key. It keeps alarming, shows FAULT, and writes
  nothing to its ledger.
- The running firmware can still ask the chip to sign anything. The secure element stops the key
  being copied, not a signed image misusing it (threat model §5).
- A locked chip cannot be re-keyed. A unit returned for repair with a faulty board gets a new
  identity.

## Not verified yet

- None of this has run on a chip. The configuration values come from the ATECC608 datasheet's
  field definitions and cryptoauthlib's configuration structure. They have not been written to a
  part, read back, or locked.
- That GenKey is refused on a locked slot, and that the public key reads back after a restart.
- Signing time, and the HAL on I2C controller 1 alongside the sensor bus on controller 0.
