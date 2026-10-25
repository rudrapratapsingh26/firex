# M6 bench: the secure element, the thermistor, and a production EVT unit

Status: written before any of it has run on hardware. The EVT PCB does not exist yet, so its
pins are provisional (`FIREX_BOARD_EVT` in `firmware/main/Kconfig.projbuild`, and
`sdkconfig.evt`). All three node images and both camera images compile. The pure-C parts are
host-tested. Nothing has touched an ATECC608B, a thermistor, or an eFuse.

M6 milestone (Software Roadmap v2.0): **an incubator-ready EVT unit with its documentation, built
on the custom PCB.** This guide rehearses everything that can be rehearsed on a dev kit first,
then provisions an EVT unit for production.

Start from a unit that passes the M5 bench checklist.

**Read this first.** Parts of this guide burn eFuses and lock the secure element. Both are
permanent. Every irreversible step is marked **IRREVERSIBLE**. Do those only on hardware you are
prepared to keep in that state for good.

## 1. What you need

| Item | Notes |
|------|-------|
| A spare ESP32-S3 dev kit for rehearsal | Not your M5 unit: the EVT partition table erases its identity, ledger and photos. |
| ATECC608B breakout boards, several | Blank (TrustCUSTOM) parts. Each provisioned chip is locked for good, so label each one. |
| A 10 k NTC thermistor (B 3950) and a 10 k resistor | For the dev-kit rehearsal of the EVT heat sensor. |
| A reference thermometer | Accurate to 0.5 °C or better. |
| The EVT board | When it exists. |
| A spare ESP32-CAM, chip revision 3.0 or later | For the camera production image. |
| The FireX release signing key | On the offline machine only (operations.md §3). The factory station needs only its digest. |
| A factory station | A PC with ESP-IDF v5.5, Python 3 and the repository. |

## 2. Rehearse on a dev kit

Wire the rehearsal parts to the spare dev kit. Everything else stays as in the M1 guide.

| From | To | Notes |
|------|----|-------|
| ATECC608B VCC / GND | 3V3 / GND | |
| ATECC608B SDA / SCL | GPIO 21 / GPIO 47 | Its own bus. Most breakouts carry pull-ups; add 4.7 k to 3V3 if not. |
| 10 k resistor | 3V3 to ADS1115 AIN2 | The fixed half of the divider (`FIREX_NTC_FIXED_OHMS`). |
| NTC thermistor | ADS1115 AIN2 to GND | `FIREX_NTC_R25_OHMS`, `FIREX_NTC_BETA`. |

Measure the 3V3 rail at the divider and set `FIREX_NTC_SUPPLY_MV` if it is not 3300.

Build and flash the EVT development image. It burns no eFuses, and a dev kit flashed with it can
go back to the dev-kit image with an erase.

```bash
cd firmware
idf.py -B build-evt -D SDKCONFIG=build-evt/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.evt" set-target esp32s3 build
idf.py -B build-evt -p <port> erase-flash flash monitor
```

Expect:
- a banner;
- the boot entry fails to write, and `status` shows a ledger fault, because the secure element
  is not provisioned yet;
- the sensors come up;
- the alarm runs.

## 3. The secure element

On the console:

```
se status
```

A blank chip reports that it is present, its serial number, its configuration zone, and the next
step, `write_config`.

Other outcomes:
- **No chip**, or a wiring fault: the chip is reported absent. Check the bus before going on.
- **A chip locked by someone else**, such as a Trust&GO or TrustFLEX part: a `refuse` step and
  the reason. The firmware will never use or provision that chip. Set it aside.

**IRREVERSIBLE** (for this chip):

```
se provision FIREX-EVT-LOCK
```

The unit reports each step as it completes:
- `write_config`;
- `lock_config`;
- `lock_data`;
- `genkey_ledger`;
- `lock_ledger_slot`.

Then it prints the ledger public key, reports success, and restarts. After the restart:

```
se status        # step done, and the same ledger public key
ledger key       # the PEM of that same key
status           # ledger open, no ledger fault
ledger tail 3    # a boot entry; its device_id is the first 16 bytes of SHA-256 over the key
```

Check again:
- `se provision FIREX-EVT-LOCK` a second time reports it is already done, with the same key, and
  changes nothing.
- Power off, remove the chip, power on. The ledger stays blocked and `status` shows FAULT. The
  alarm still runs. Refit the chip and the key is back.

Export the ledger and verify it on a laptop (m2-bench.md), to show that signatures from the chip
verify:

```bash
python tools/ledger/export_log.py --port <port> --out captures/evt-rehearsal
npm run verify -- captures/evt-rehearsal.json
```

## 4. The thermistor

- `status` shows `ntc` ready and `dht22` ready. On this image the DHT22 reports humidity only.
- `read` shows the thermistor temperature beside the DHT22's.

Follow [calibration.md](../calibration.md) for the heat sensor. At minimum:
1. Let the room settle, and compare the thermistor with the reference thermometer held beside it.
   Record both.
2. Capture while warming the thermistor gently with a hand, or with warm air from a distance.
   Never use a flame near the MQ heaters (the §4 safety rules of the M1 guide).

   ```bash
   python tools/capture/capture.py --port <port> --out captures/ntc-response --period 500
   ```

3. In the CSV, the thermistor's temperature rises visibly ahead of the DHT22's.
4. Disconnect the thermistor while running. The heat score turns invalid, not zero, and a `fault`
   entry follows. Reconnect it.

## 5. Build and sign the images

On the release machine, after bumping `PROJECT_VER` (operations.md §4):

```bash
cd firmware
idf.py -B build-evt-production -D SDKCONFIG=build-evt-production/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.evt;sdkconfig.production" \
       set-target esp32s3 build
python ../tools/ota/sign.py sign --key <key> build-evt-production/bootloader/bootloader.bin
python ../tools/ota/sign.py sign --key <key> build-evt-production/firex_node.bin
python ../tools/ota/sign.py check build-evt-production/bootloader/bootloader-signed.bin
python ../tools/ota/sign.py check build-evt-production/firex_node-signed.bin     # also prints kind evt-production
python ../tools/ota/sign.py digest build-evt-production/firex_node-signed.bin    # the value for --release-key-digest
```

Each image must show one signature block, with the release key's digest. Copy both signed images,
the partition table and `flasher_args.json` to the factory station, with the digest. Never copy the
key.

## 6. The factory run

Before a real EVT board, run this once on a rehearsal board you are prepared to keep burned: a dev
kit with the breakout, built with the production image.

**IRREVERSIBLE** (for the board and the chip):

```bash
python tools/factory/provision.py unit --port <port> \
       --build firmware/build-evt-production \
       --signed-bootloader firmware/build-evt-production/bootloader/bootloader-signed.bin \
       --signed-app firmware/build-evt-production/firex_node-signed.bin \
       --release-key-digest <digest> --unit-serial EVT-0001 --records <records dir> \
       [--operator <name>] [--station <name>] \
       [--camera-port <port> --camera-build firmware/cam/build-production \
        --camera-signed-bootloader <file> --camera-signed-app <file>]
# --dry-run prints the commands, offsets and checks without opening a port
```

The tool:
1. Reads the chip's eFuses. It refuses a board already secured with a different key, and resumes a
   board it secured itself.
2. Checks that both signed images carry exactly one signature block, with the expected digest.
3. Writes the signed bootloader, the partition table, the OTA data and the signed application.
4. Waits for the first boot. The bootloader and the application burn:
   - the secure boot key digest, revoking the unused digest slots;
   - a flash encryption key, then encrypt the flash;
   - JTAG and USB JTAG disable;
   - secure download mode;
   - the NVS HMAC key.

   This takes longer than a normal boot.
5. Runs `security status` and checks every line against the expected production state.
6. Provisions the secure element (§3) and waits for the restart.
7. Reads:
   - the ledger key, which must match the secure element's report;
   - the pairing code;
   - `status`.
8. Appends a record to `manifest.jsonl` and the pairing code to `labels.csv`.

Then:

```bash
python tools/factory/provision.py verify-manifest <records dir>/manifest.jsonl
```

Print the label, check its code against `pairing` on the unit, and delete `labels.csv`
(operations.md §6).

## 7. Check the production locks

On the provisioned unit:

```
security status
```

Expect:
- secure boot on;
- flash encryption release;
- JTAG and USB JTAG disabled;
- ROM download secure;
- key digest 0 equal to the release key's, with the other slots revoked;
- NVS encryption on;
- the device key in the ATECC608B;
- production yes;
- the firmware version and hash.

```
config set cam_frames 5      # refused: fixed in production firmware
config get cam_frames        # 3
media get x                  # no such subcommand; `help` lists media as `list` only
inject flame 5               # unknown command
```

Then an update:
1. Build the next production version, sign it, and roll it out from a gateway (m5-bench.md §6).
2. It confirms.
3. Add an EVT development image, signed with the same key, and roll it out to the production unit.
   Expect `fw_update failed` with "the image is for another kind of unit (dev kit, EVT or EVT
   production)" as soon as the download starts. The unit keeps running its production image
   (ADR 0009).

With esptool, `read_flash` and `dump_mem` are refused in secure download mode.

## 8. The camera production image

1. Check the module's revision first:

   ```bash
   esptool.py --chip esp32 -p <camera port> chip_id     # "revision v3.0" or later
   ```

2. Build and sign:

   ```bash
   cd firmware/cam
   idf.py -B build-production -D SDKCONFIG=build-production/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.production" set-target esp32 build
   python ../../tools/ota/sign.py sign --key <key> build-production/bootloader/bootloader.bin
   python ../../tools/ota/sign.py sign --key <key> build-production/firex_cam.bin
   ```

3. **IRREVERSIBLE**, and the camera image can never be changed afterwards (ADR 0009). Flash the
   signed bootloader, the partition table and the signed application once, at the offsets in
   `build-production/flasher_args.json`. Let it boot; the first boot takes longer while it
   encrypts.
4. Check it:
   - on the camera's USB console, `PING` answers with a link-only image;
   - `SNAP` is refused;
   - from the node, `cam ping` answers, and `selftest` passes the camera check;
   - a lighter flame at the §4 distance gives a CRITICAL with frames.

## 9. M6 exit checklist

- [ ] The EVT development image runs on the dev kit with the breakout: sensors, alarm, camera.
- [ ] A blank ATECC608B provisions. The key survives a restart, and the unit refuses a second
      provisioning and a foreign chip.
- [ ] Ledger entries signed by the secure element verify with the reference verifier.
- [ ] The thermistor reads within the calibration tolerance of a reference thermometer, leads the
      DHT22 on a warm-up, and reports invalid when disconnected.
- [ ] The production image passes the factory tool on a rehearsal board, and `security status`
      matches every expected line.
- [ ] `verify-manifest` passes. The label's pairing code matches the unit. The label file is
      deleted.
- [ ] The production locks hold: `cam_frames` fixed, no `media get`, no bench commands, no flash
      read over serial.
- [ ] A signed production update confirms on the production unit.
- [ ] A signed EVT development image offered to the production unit is refused, and the unit keeps
      running.
- [ ] The camera production image boots, refuses `SNAP` on its console, and captures on CRITICAL.
- [ ] All of the above repeated on the EVT PCB, with its real pin map written back into Kconfig and
      `sdkconfig.evt`.
- [ ] The documentation set is current: architecture, threat model, calibration, operations, ADRs
      0008 and 0009.

## 10. Not verified yet

- **The PCB.** It does not exist. Every EVT pin is a placeholder, the ATECC608B on GPIO 21/47
  included.
- **The secure element.** Its configuration bytes have never been written to a real chip, read
  back, or locked. That GenKey is refused on a locked slot is from the datasheet only.
- **cryptoauthlib's ESP32 HAL** on I2C controller 1, beside the sensor bus on controller 0.
- **The bootloader size.** 0xA000 with secure boot and flash encryption comes from one trial build.
- **First boot.** How long the encryption takes, what is burned in which order, and what a power
  cut during it does.
- **NVS encryption.** The HMAC key generation and burn in eFuse key block 5.
- **Secure download mode** with the factory tool: which esptool and espefuse operations still work
  after the first boot.
- **The camera.** The chip revision of the modules on hand, and its production image on any board.
- **The thermistor.** Accuracy, response time in the enclosure, and self-heating from the divider
  current.
- **Signing time** on the chip, and its effect on the ledger queue during a burst of events.
