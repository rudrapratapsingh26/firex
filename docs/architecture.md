# FireX architecture

Status: draft, M6. This describes the code in this repository through M6. None of it has run
on a board, and the EVT board has no schematic yet, so its pin map is provisional. Where a
decision has its own record, this page links to it rather than repeating it.

The rule from Software Architecture v1.0 still governs everything below: **the alarm never
depends on the network.** The camera, the gateway, escalation and anchoring all sit downstream
of a buzzer that has already sounded.

## 1. Four layers

| Layer | Runs on | Code | State in M6 |
|-------|---------|------|-------------|
| Node | ESP32-S3, plus an ESP32-CAM as a second MCU | `firmware/`, `firmware/cam/` | Dev kit and EVT images compile; host tests run in CI |
| Site gateway | Raspberry Pi 4/5 on the site LAN | `gateway/`, `dashboard/`, `infra/` | Runs on a laptop against simulated units |
| Anchor | NBFLite sandbox (MeitY Vishvasya) | not built | Q3. Nothing leaves the site today |
| Verifier | Any computer, offline | `ledger/src` (CLI) | Built; the static page is Q3 |

A node with no Wi-Fi is still a working detector with a working log. The gateway adds
mirroring, verification on receipt, the dashboard, escalation and the site chain.

## 2. The node

### 2.1 Tasks

From `firmware/main/node.h`, highest priority first, by what must still work when everything
else has failed:

| Task | Priority | Job |
|------|----------|-----|
| alarm | 10 | Buzzer and ring from shared state. Never waits on a queue. |
| fusion | 8 | Sensors, then the engine, then events and shared state, every 500 ms. |
| sensors | 6-7 | One task per sensor driver. |
| ledger | 5 | Drains the event queue: hash, sign, store. |
| camera | 3 | Capture on the transition into CRITICAL only. |
| reporter | 3 | Serial stream for `tools/capture`, fan health. |
| selftest | 2 | Daily checks. |
| console | 2 | The REPL. |
| net | 2 | Wi-Fi, provisioning, MQTT to the gateway. |

The trace task and the OTA task (M5) are in `trace_task.c` and `ota_task.c`. Alarm, fusion and
sensor tasks are on the task watchdog. Producers queue ledger events with a zero timeout: a full
queue raises the ledger fault rather than block an alarm path (ADR 0004).

Start-up order is in the header comment of `firmware/main/node_main.c`. Keys are opened before
any ADC driver starts, because generating a software key uses the SAR ADC as an entropy source.

### 2.2 Boards and images

One source tree builds three node images. Each is a stack of `sdkconfig` fragments, later
files overriding earlier ones:

| Image | `SDKCONFIG_DEFAULTS` | Board | Heat | Device key | Partition table | Security |
|-------|----------------------|-------|------|------------|-----------------|----------|
| Dev kit | `sdkconfig.defaults` | ESP32-S3-DevKitC-1 | DHT22 | NVS | `partitions.csv` | Signed updates only (M5) |
| EVT | `+ sdkconfig.evt` | EVT PCB | NTC thermistor | ATECC608B | `partitions_evt.csv` | Signed updates only |
| EVT production | `+ sdkconfig.production` | EVT PCB | NTC thermistor | ATECC608B | `partitions_evt.csv` | Secure boot v2, flash encryption, JTAG off, NVS encryption, privacy locks |

The build commands are in the header of each fragment. The board, heat sensor, key location and
production switch are Kconfig choices in `firmware/main/Kconfig.projbuild` (`FIREX_BOARD_*`,
`FIREX_HEAT_*`, `FIREX_KEY_*`, `FIREX_PRODUCTION`). The EVT image is for bring-up and never
burns eFuses; only the production image does. The reasoning is in
[ADR 0009](adr/0009-secure-boot-and-production-locks.md).

The camera has two images the same way: `firmware/cam/sdkconfig.defaults`, and
`sdkconfig.production` on top of it.

### 2.3 Sensors and the score API

Every driver sits behind `fx_sensor_t` (`components/sensors/include/fx_sensor.h`): `init`,
`poll` in the driver's own task, `score` from cached values only, and `raw` for the capture
stream. Each driver declares the modalities it owns, and the collector drops anything a driver
writes outside them. A failed or silent sensor scores **invalid, never zero**. The pure scoring
half is host-tested; the I/O half is ESP-IDF code ([ADR 0002](adr/0002-sensor-drivers-split-scoring-from-io.md)).

| Modality | Dev kit | EVT |
|----------|---------|-----|
| Gas | MQ-2 on ADS1115 AIN0 | same |
| Smoke / air quality | MQ-135 on ADS1115 AIN1 | same |
| Flame | KY-026, flicker in 1-15 Hz on an ADC1 pin | same |
| Heat | DHT22 temperature, 2 s floor | NTC thermistor on ADS1115 AIN2, every 500 ms |
| Humidity (context, never a voter) | DHT22 | DHT22, humidity only |

The EVT thermistor is a divider: a fixed resistor from the supply to AIN2 and the thermistor
from AIN2 to ground, converted with the B-parameter equation. A reading outside the part's
range, as from an open or shorted thermistor, is invalid. The heat score keeps the same two terms
as on the dev kit, absolute temperature and rate of rise; the calibration procedure is in
[calibration.md](calibration.md). The fusion rules do not change with the board.

### 2.4 Alarm, camera and self-test

- **Alarm.** A state machine from fusion state to buzzer (temporal-three) and status ring; a
  physical silence button, never a console command.
- **Camera.** The ESP32-CAM is a second MCU behind a framed UART protocol, on the EVT board too
  ([ADR 0003](adr/0003-esp32-cam-as-a-second-mcu.md)). The node asks for frames only on the
  transition into CRITICAL, hashes each as it arrives, and writes the hashes into
  `media_captured`. The rules are in [media-policy.md](media-policy.md).
- **Self-test.** Daily: heater plausibility, buzzer chirp, flame loopback, fan, a camera `CHECK`
  whose image never leaves the camera, and the ledger's links.

### 2.5 The ledger on the node

Canonical CBOR entries, SHA-256 chain, ECDSA P-256 signature, CRC-checked records in rotating
segment files on a LittleFS partition ([ADR 0004](adr/0004-on-device-ledger.md),
[ledger/SPEC.md](../ledger/SPEC.md)). The signer is a function pointer, so the chain writer is the
same whether the key is in NVS or in the secure element.

### 2.6 Keys and secrets

| Secret | Algorithm | Generated | Dev kit | EVT development | EVT production | Used for |
|--------|-----------|-----------|---------|-----------------|----------------|----------|
| Device key | ECDSA P-256 | On the unit | NVS, plain | ATECC608B slot 0, never exported | same | Signing ledger entries; the provisioning proof |
| TLS key | ECDSA P-256 | On the unit | NVS, plain | NVS, plain | NVS, encrypted | MQTT and firmware download, mutual TLS |
| Pairing code | 50 random bits | On the unit | NVS, plain | NVS, plain | NVS, encrypted | Proof of possession at onboarding and provisioning |
| Site CA key | ECDSA P-256 | On the gateway | Gateway data volume | same | same | Unit and broker certificates, the CRL |
| Site-ledger key | ECDSA P-256 | On the gateway | Gateway data volume | same | same | The site chain |
| Release signing key | RSA-3072 | Offline, once | Offline | Offline | Offline; its digest in eFuse | Signing firmware images |
| Flash encryption key | AES | On the chip, first boot | none | none | eFuse, read-protected | Flash contents |
| NVS HMAC key | HMAC-SHA256 | On the chip, first boot | none | none | eFuse key block 5 | Deriving the NVS encryption keys |

Why the device key moved and the TLS key did not is in
[ADR 0008](adr/0008-secure-element.md). Custody and loss of each key are in
[operations.md](operations.md), and what each protects against is in
[threat-model.md](threat-model.md).

### 2.7 Partition tables

Dev kit (`firmware/partitions.csv`), laid out in M5 so a unit kept its NVS, ledger and photos
through the one serial upgrade:

| Name | Offset | Size |
|------|--------|------|
| nvs | 0x9000 | 0x6000 |
| phy_init | 0xF000 | 0x1000 |
| ota_0 | 0x10000 | 0x200000 |
| ledger | 0x210000 | 0x100000 |
| media | 0x310000 | 0x300000 |
| otadata | 0x610000 | 0x2000 |
| ota_1 | 0x620000 | 0x1E0000 |

EVT (`firmware/partitions_evt.csv`), with the table at 0x10000 because the secure boot and flash
encryption bootloader does not fit before 0x8000:

| Name | Offset | Size | Flags |
|------|--------|------|-------|
| nvs | 0x11000 | 0x6000 | |
| phy_init | 0x17000 | 0x1000 | |
| otadata | 0x18000 | 0x2000 | |
| ota_0 | 0x20000 | 0x1F0000 | |
| ota_1 | 0x210000 | 0x1F0000 | |
| ledger | 0x400000 | 0x100000 | encrypted |
| media | 0x500000 | 0x300000 | encrypted |

The `encrypted` flag takes effect only when flash encryption is on, so the same table serves both
EVT images.

### 2.8 The console

A REPL on the UART, documented at the top of `firmware/main/console.c`. Every command that
changes something is logged. There is deliberately no command that silences the alarm or
triggers the camera. M6 adds `se` and `security` (`console_security.c`). A production image has
no `media get` and no bench commands, and refuses `config set cam_frames`.

## 3. Node to gateway

All of it is in [transport.md](transport.md):

- **Provisioning.** A pairing code from the label, a proof signed with both unit keys, and a
  reply MAC so a rogue gateway cannot hand the unit its CA (§1).
- **MQTT over mutual TLS.** The unit's certificate is its identity at the broker, and the broker's
  access rules limit it to its own topics (§2).
- **Store and forward.** The ledger on flash is the outbound queue; the gateway acknowledges by
  seq (§3).
- **Clock.** The unit takes its time from the gateway over the authenticated link (§5).
- **Firmware updates.** Signed images, offered one unit at a time, confirm within 60 s or roll
  back (§6, [ADR 0007](adr/0007-pilot-sites.md)). From M6 an image also names its kind (`devkit`,
  `evt`, `evt-production`) inside the signed bytes, and a unit refuses an update of another kind
  ([ADR 0009](adr/0009-secure-boot-and-production-locks.md)).
- **Traces and photos.** Travel on their own topics named by their SHA-256, and are deleted on
  the unit only once the gateway has stored and checked them (§7, media policy).

## 4. The site gateway

A Compose stack on the Pi ([infra/README.md](../infra/README.md),
[ADR 0005](adr/0005-site-gateway.md)):

- **Mosquitto.** The broker: mutual TLS, per-unit access rules, the site CRL.
- **firex-server.** Ingest and verification on receipt, the SQLite database, provisioning and the
  site CA, the HTTP API, the site chain, escalation, heartbeat gaps, the firmware store and
  rollouts, traces, retention and the pilot export.
- **Caddy.** Local HTTPS for the dashboard.

The gateway writes its own **site chain**: alerts, acknowledgements, timeouts, gaps, drills,
registry changes, firmware offers and unit removals, in the ledger's entry format, signed by the
gateway's key ([ADR 0007](adr/0007-pilot-sites.md), SPEC §3.5). Escalation runs behind the
`AlertProvider` interface, with console, HTTP and GSM modem providers, and services stay in drill
mode until an agreement exists ([service-registry.md](service-registry.md)).

The dashboard (`dashboard/src/views`) covers:

- the site plan and each unit;
- the timeline and incidents with their escalations;
- drills and pairing;
- firmware;
- the unit ledgers and the site chain;
- the service registry.

## 5. Verification

`npm run verify -- <bundle>` checks sequence, chain, signature and media hashes, and names the
first failing entry and the tier that failed ([ledger/SPEC.md](../ledger/SPEC.md) §2 and §6).
It needs no account, no server and no FireX involvement. A unit bundle and a site chain verify
with the same code. Secure element signatures (EVT) and software signatures (dev kit) verify the
same way.

## 6. Tools

| Tool | Milestone | Job |
|------|-----------|-----|
| `tools/simulator` | M0 | YAML scenarios through the compiled C engine |
| `tools/capture`, `tools/calibration`, `tools/camera` | M1 | Serial capture, characterisation notebooks, JPEG pull and checks |
| `tools/ledger` | M2 | Export a unit's ledger and frames into a bundle |
| `tools/validation` | M4 | Trial replay, burn-in, alert latency, the reliability report |
| `tools/ota` | M5 | Release key and image signing |
| `tools/pilot` | M5 | The pilot report across site exports |
| `tools/factory` | M6 | Production provisioning of an EVT unit and its factory records |

## 7. Repository

The directory map is in the [README](../README.md#layout). The decisions behind the layout are
in [docs/adr](adr). Bench procedures, milestone by milestone, are in
[docs/bringup](bringup/README.md).

## 8. Not built

- Anchoring roots to NBFLite, the public verifier page, media manifests in the anchor (Q3).
- Fleet view and staged OTA groups beyond one-unit-at-a-time rollouts (Q3).
- The EVT PCB itself; a production-grade gas sensor. The only EVT sensor change is the thermistor.
- A hardware key store for the gateway (not planned before Q3, ADR 0007).
- Firmware updates for the camera MCU (ADR 0009).
