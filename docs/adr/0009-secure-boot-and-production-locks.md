# ADR 0009 — Secure boot, flash encryption and the production locks

Date: 2026-09-16
Status: accepted for M6

## Context

M6 is the port to the custom PCB. The roadmap asks for:
- secure boot v2 and flash encryption on;
- debug interfaces disabled in production fuses;
- the camera on the production board, with the privacy defaults locked in firmware rather than
  configuration.

ADR 0007 signed every M5 image with Espressif's secure boot v2 scheme and an offline RSA-3072 key,
and said M6 would turn on secure boot with the same key. The media policy says its rules are
enforced "in firmware, not in a configuration file a site can change". Some were still settings
or bench options.

There is no schematic for the EVT board. The dev kit must keep building, and EVT bring-up needs an
image that burns nothing. The camera stays an ESP32-CAM module, a second MCU behind the same UART
protocol, now on the PCB (ADR 0003).

## Decisions

**Three node images from one tree.** Layered `sdkconfig` fragments, each with its build command
in its header:

| Image | Fragments | Burns eFuses |
|-------|-----------|--------------|
| Dev kit | `sdkconfig.defaults` | No |
| EVT development | `+ sdkconfig.evt` | No |
| EVT production | `+ sdkconfig.production` | Yes, at first boot |

CI builds all three, unsigned. The release key never reaches CI.

**Secure boot v2 with the M5 release key.**
- RSA-3072, the same key and signature scheme as M5 updates.
- The bootloader's signature block carries the key. At first boot its digest is burned into
  eFuse, and the unused digest slots are revoked (ESP-IDF's default).
- From then on the ROM checks the bootloader, and the bootloader checks the application, at every
  boot. Updates keep the M5 check before a slot switch.
- Images are built unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`) and signed offline
  with `tools/ota/sign.py`.

**Flash encryption in release mode.** The key is generated on the chip at first boot and
read-protected. Plaintext can no longer be written over serial, and flash contents read back only
as ciphertext.

**Debug interfaces off.**
- The JTAG pad and USB JTAG are disabled: `CONFIG_SECURE_BOOT_ALLOW_JTAG` is left unset.
- The ESP32-S3's ROM serial loader keeps only secure download mode, which can neither read flash
  nor run code in RAM.

**NVS encryption with the HMAC scheme.** NVS holds the Wi-Fi password, the pairing code, the TLS
key and the site certificates, and NVS cannot take the partition table's `encrypted` flag. The
production image turns on NVS encryption with keys derived from an HMAC key in eFuse key block 5.
The app generates and burns that key at first boot; the flash encryption key and the secure boot
digest take the first free blocks before it.

**A new partition table for the EVT board.** A trial build showed the bootloader with secure boot
v2 and flash encryption is 0xA000 bytes, past the default table offset of 0x8000. The EVT table
(`partitions_evt.csv`) starts at 0x10000:

| Name | Offset | Size | Flags |
|------|--------|------|-------|
| nvs | 0x11000 | 0x6000 | |
| phy_init | 0x17000 | 0x1000 | |
| otadata | 0x18000 | 0x2000 | |
| ota_0 | 0x20000 | 0x1F0000 | |
| ota_1 | 0x210000 | 0x1F0000 | |
| ledger | 0x400000 | 0x100000 | encrypted |
| media | 0x500000 | 0x300000 | encrypted |

- **App slots.** The two are equal. The M5 layout had a smaller second slot only to keep dev-kit
  offsets.
- **Encrypted flag.** It takes effect only when flash encryption is on, so the table serves both
  EVT images.
- **The dev kit** keeps `partitions.csv` unchanged.

**The build refuses a production image without its locks.** With `CONFIG_FIREX_PRODUCTION` set,
`node_main.c` stops the build with `#error` unless:
- secure boot v2 is on;
- flash encryption is on, in release mode;
- the device key is the ATECC608B (ADR 0008);
- NVS encryption is on;
- JTAG and the insecure ROM loader stay off;
- the bench injection is absent.

A mistake fails the build, not a unit in a hostel.

**Privacy locks in firmware.** In a production image:

| Rule (media policy) | Before M6 | Production image |
|---------------------|-----------|------------------|
| A fixed number of frames per incident | `cam_frames` setting, 0-5, changeable with `config set` | Fixed at build time by `CONFIG_FIREX_CAM_FRAMES_LOCKED` (default 3); `config set cam_frames` is refused |
| Photos stay on the site | `media get` could copy a photo off the unit over USB | `media get` does not exist; photos leave only to the gateway |
| No remote shutter | Bench options `FIREX_BENCH_INJECT` and the camera's `FIREX_CAM_BENCH_SHUTTER` | Neither can be selected (Kconfig `depends on`), and the build refuses them |
| Only FireX firmware enforces the rules | Any image could be flashed | Only images signed with the release key boot |
| Only production firmware replaces production firmware | An update needed only the release key's signature | The update's image kind must match the unit's, so a signed dev kit or EVT development image is refused |

**Every image names its kind.** The three node images are signed with the same key and share the
project name, so a signature alone cannot tell them apart. From M6 each image carries a 32-byte
record in ESP-IDF's custom app description, at offset 288 right after `esp_app_desc_t` and inside
the signed bytes: the magic `FXKIND1` and the kind `devkit`, `evt` or `evt-production`
(`main/image_kind.c`). `ota_task.c` reads it from the first bytes of a download and refuses an
image whose kind differs from its own, or that has none (`fx_ota_kind_refusal`, host-tested in
`test_ota.c`). Images built before M6 have no record and are refused by M6 units; M5 units, which
do not check, still take M6 dev kit images.

**The camera gets its own production image.** The ESP32-CAM enforces the rule that capture is
never accepted from its console, and it has a Wi-Fi radio of its own. A reflashed camera could
take and send photos without the node. `firmware/cam/sdkconfig.production` adds:
- secure boot v2 with the same key, which needs ESP32 chip revision 3.0 or later;
- flash encryption in release mode;
- the ROM loader disabled outright. The ESP32 has no secure download mode, and an open loader runs
  unsigned code in RAM.

**The first boot does the burning, and only the factory tool starts it.** `tools/factory` writes
the signed images to a blank board and waits for the first boot. It then checks the result with
`security status` before provisioning the secure element (ADR 0008).

## Rejected

- **Development-mode flash encryption for production.** It leaves plaintext reflashing over
  serial available, which undoes secure boot's point for anyone with a cable.
- **Leaving the camera module unsecured.** It carries the console-shutter refusal and a radio; the
  enclosure alone is not a lock.
- **Keeping the dev kit's partition offsets.** They cannot hold the larger bootloader. An EVT board
  starts blank, so there is no unit to keep compatible.
- **Burning a second, backup key digest.** It would allow rotating away from a compromised key,
  but a second key is a second secret to keep offline for the unit's life. Kept simple for EVT;
  revisit before a production run.
- **Locks as settings a site cannot see.** The media policy's point is that a site cannot change
  them, so they are compiled in.

## Consequences

- **No serial reflash.**
  - A production unit cannot be reflashed over serial or debugged with JTAG.
  - A production board that will not boot a valid image is scrap.
  - EVT bring-up uses the development image on unburned boards first.
- **The release key matters more.**
  - Losing it means no updates for any unit, and no way to make a production unit boot a
    different image.
  - A compromise cannot be rotated out of units already burned (operations.md §3).
- **The camera image is fixed for the life of the unit.** The camera has no network and no OTA,
  and its loader is disabled, until a signed update over the node link exists. Camera firmware
  changes must be finished before a production run.
- **Chip revision.** A camera module below revision 3.0 cannot take the production image. Check
  the revision before buying modules.
- **First boot takes longer** while the chip encrypts its flash (not measured).
- **Secrets at rest.** NVS encryption on production units closes flash-dump access to the Wi-Fi
  password and the TLS key. The dev kit and the EVT development image keep them in plain NVS.
- **An image of the wrong kind fails at download.** A rollout that offers a dev kit or EVT
  development image to a production unit ends in `fw_update failed` for that unit, and the unit
  keeps running. The kind check trusts the release key: an image signed with it can claim any
  kind, so the kind protects against mistakes and gateway misuse, not against the key holder.

## Not verified yet

- The bootloader size comes from one trial build, without the application. The EVT table and both
  EVT images have not been flashed.
- The first-boot sequence on the S3:
  - which eFuses are burned, and in what order;
  - how long encryption takes;
  - what happens on power loss during it.
- NVS encryption's HMAC key generation in block 5.
- Secure download mode's behaviour with the factory tool.
- The camera module's chip revision, and its production image on any board.
