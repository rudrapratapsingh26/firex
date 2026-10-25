# FireX

A multi-sensor hazard-detection node: five hazard signals on one device, a critical alarm
only when two independent signals agree, a camera that captures the room when that
happens, alerts to the fire station, police station and ambulance that cover the site, and
every event written to a tamper-evident record a public buyer can verify.

**Stage: TRL 2.** M0 of the software roadmap is done: the work that needs a laptop and a
₹2,200 sensor kit, and no hardware, funding or incorporation. M1 through M6 are written and
host-tested, and the firmware images compile, but **nothing has run on a board yet**:

- M1: bring-up.
- M2: alarm, on-device ledger, camera trigger, self-test.
- M3: site gateway, dashboard, provisioning, ledger mirroring over mutual TLS.
- M4: validation tooling. Trial replay with error rates, the burn-in runner, camera checks,
  escalation dry runs and the reliability report.
- M5: what a pilot site runs:
  - signed firmware updates with automatic rollback;
  - the unit's clock taken from its gateway, and raw traces around every episode;
  - live escalation over a GSM modem, with services in drill mode until they agree;
  - the gateway's own signed site chain, and unit removal with certificate revocation;
  - the pilot export and the pilot report.
- M6: the port to the EVT board, whose schematic does not exist yet, so its pins are
  provisional:
  - the device key generated in an ATECC608B that FireX configures and locks;
  - an NTC thermistor for heat, behind the same score API;
  - secure boot v2 with the release key, flash encryption, JTAG off, and encrypted NVS in a
    production image;
  - privacy locks compiled in: a fixed frame count, no `media get`, no bench options;
  - a production image for the camera module;
  - the factory provisioning tool;
  - the documentation set: [architecture](docs/architecture.md),
    [threat model](docs/threat-model.md), [calibration](docs/calibration.md) and
    [operations](docs/operations.md).

The gateway, dashboard and every M4 and M5 tool have run on a laptop against simulated units,
a fake modem and synthetic data. The bench procedures and exit checklists are in
[docs/bringup](docs/bringup/README.md) (M1 to M6). The M4 measurement protocol is
[docs/validation.md](docs/validation.md), and the pilot protocol is
[docs/pilot.md](docs/pilot.md).

## What works today

```bash
npm install                 # the verifier needs none of it at runtime; the gateway does
npm test                    # 155 TypeScript tests: ledger, verifier, gateway, escalation, site chain, OTA, traces, pilot export
make -C firmware test       # 176 Unity tests: fusion, sensors, ledger, alarm, media, core (and core as a production image builds it), net, ota, trace, secure element
make -C firmware bundle && npm run verify -- firmware/host-build/golden-bundle.json
                            # the C ledger's export, checked by the reference verifier
make -C firmware sim        # build the engine harness the simulator drives
python tools/simulator/simulate.py     # twelve scenarios through the real C engine, the nuisance matrix among them
npm run demo                # the M0 milestone, below
npm run demo:escalation     # one alarm, four recipients, one silent service
npm run vectors             # regenerate ledger/vectors/ (a new key each time)
npm run tools:test          # 169 Python tests: capture, camera, ledger export, calibration, validation, OTA signing, pilot report, factory provisioning
npm run dashboard:build     # the dashboard, served by the gateway
npm run gateway             # the site gateway; the laptop rehearsal is in docs/bringup/m3-bench.md
python tools/validation/replay.py --demo --out reports/replay-demo     # M4: synthetic trials replayed, with rule sweeps
npm run escalation:dry-run  # M4: simulated units, the gateway's escalation, mock fire, police and ambulance endpoints
python tools/pilot/pilot_report.py --demo reports/pilot-sites --out reports/pilot-demo   # M5: two synthetic sites
```

With ESP-IDF v5.5 installed, the firmware images. Each node image is a stack of `sdkconfig`
fragments in its own build directory:

```bash
cd firmware
idf.py set-target esp32s3 build                                   # dev kit (sign with tools/ota/sign.py before flashing)
idf.py -B build-evt -D SDKCONFIG=build-evt/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.evt" set-target esp32s3 build      # EVT board, burns nothing
idf.py -B build-evt-production -D SDKCONFIG=build-evt-production/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.evt;sdkconfig.production" set-target esp32s3 build
                                                                  # EVT production: burns eFuses at first boot (docs/bringup/m6-bench.md)
cd cam && idf.py set-target esp32 build                           # the ESP32-CAM; sdkconfig.production for its production image
```

### The M0 milestone

`npm run demo` builds a signed incident log — a gas leak that becomes a fire, with photos
and three alerts — verifies it, then edits one field and verifies it again:

```
  as exported:   PASS  13 entries, 2026-09-10T00:26:40Z to 2026-09-10T00:43:32Z
  after an edit: TAMPERED at entry 6 (chain): hash does not match the entry's own contents
```

## Layout

```
firmware/               ESP-IDF project for the ESP32-S3 node
  sdkconfig.defaults    the dev kit; sdkconfig.evt adds the EVT board, sdkconfig.production its eFuse and privacy locks
  partitions.csv        dev-kit partitions; partitions_evt.csv the EVT table at 0x10000 (docs/adr/0009)
  components/fusion/    pure C11 engine: five scores in, alarm states out (no hardware deps)
  components/sensors/   score/: pure-C scoring, host-tested; hw/: ESP-IDF drivers, the EVT thermistor among them (docs/adr/0002)
  components/ledger/    the ledger on the node: CBOR, chain, ring buffer on LittleFS, device key (docs/adr/0004)
  components/secure/    the ATECC608B: configuration and lock rules (pure C), provisioning, signing, eFuse status (docs/adr/0008)
  components/alarm/     alarm state machine, temporal-three buzzer, status ring, silence button
  components/media/     camera link: replies parsed and JPEGs hashed as they arrive
  components/core/      ledger payloads, settings schema, self-test judgements, the clock
  components/net/       pairing codes, provisioning exchange, store and forward, gateway commands (docs/transport.md)
  components/ota/       update acceptance, image kind, confirm-or-roll-back, warm restart (docs/adr/0007, 0009)
  components/trace/     episode traces: the pre-window, the episode rules, the file format
  main/                 the node: alarm, fusion, ledger, camera, self-test, reporter, trace, OTA and network tasks, console;
                        a bench-only build adds `inject flame` for the burn-in (docs/adr/0006)
  cam/                  ESP32-CAM image: captures for the node only (docs/adr/0003); sdkconfig.production locks it
  sim/                  line protocol around the engine; golden bundle and golden trace generators
  test/                 Unity host tests
ledger/
  SPEC.md               entry format, hash and signature construction, event types, the site chain
  src/                  canonical CBOR, entry signing, verifier, CLI, vector generator
  vectors/              ten test vectors: one valid chain, nine broken
gateway/
  server/               firex-server: MQTT ingest, provisioning, site CA and revocation, HTTP API, site chain,
                        escalation runner, heartbeat gaps, firmware store and rollouts, traces, pilot export, CLI
  sim/                  simulated units, a TLS dev broker with the site's access rules, golden files
  escalation/           AlertProvider, console, HTTP and GSM modem providers, the escalation service,
                        the site registry, messages, mock endpoints and the dry run
dashboard/              site plan, units, incidents with their escalations, drills, firmware, site chain, registry
infra/                  Compose stack for a Raspberry Pi: firex-server, Mosquitto, Caddy
tools/simulator/        YAML scenarios driven through the real engine; synthetic captures for the replay harness
tools/capture/          record the node's serial stream to CSV, with markers
tools/camera/           pull a JPEG from the ESP32-CAM and check its SHA-256; frame metrics for the camera checks
tools/calibration/      warm-up, baseline, fan-response and RH notebooks (fxcal.py underneath)
tools/ledger/           export the node's ledger and frames into a bundle for the verifier
tools/validation/       M4: trial replay and error rates, burn-in runner and report, alert-path latency, the report
tools/ota/              M5: signing keys and image signing for firmware updates
tools/pilot/            M5: the pilot report across site exports
tools/factory/          M6: production provisioning of an EVT unit, and the chained factory manifest
docs/                   architecture, threat model, operations, calibration, transport, media policy,
                        pilot and validation protocols, service registry, bring-up guides, architecture decisions
```

## Stack

| Layer | Choice | Why |
|-------|--------|-----|
| Fusion engine | C11, no dependencies | The same code runs on the node, in CI, under the simulator and under the replay harness; it has no hardware includes, so it compiles anywhere. |
| Firmware (M1+) | ESP-IDF on FreeRTOS, ESP32-S3 | Signed updates to A/B slots with rollback; from M6, secure boot v2 and flash encryption with the same key. Not Arduino. |
| Secure element (M6) | ATECC608B through Espressif's esp-cryptoauthlib | The device key is generated in the chip and never leaves it (docs/adr/0008). |
| Host tests (C) | Unity | Vendored in `firmware/test/unity`, built by a plain Makefile into `firmware/host-build/`. |
| Ledger, verifier, gateway | TypeScript on Node 22 | One language across gateway, dashboard and verifier for a one-person team. |
| Crypto | `node:crypto` (SHA-256, ECDSA P-256), RSA-PSS checked in BigInt | The verifier and the gateway's image check have no runtime dependencies. On the node the same primitives come from mbedTLS. |
| Gateway | Fastify, mqtt.js, `node:sqlite`, @peculiar/x509 | One process and one database file on a Pi; the site CA and its revocation list are created on the gateway (docs/adr/0005). |
| Transport | MQTT 3.1.1 over mutual TLS, Mosquitto 2 | Each unit's certificate is its identity at the broker; the ledger on flash is the outbound queue. |
| Alerts | A USB GSM modem on the gateway, AT commands over a serial device | SMS and voice with no internet, no aggregator account and no native module (docs/adr/0007). |
| Dashboard | React and Vite, fonts bundled | Works on a site LAN with no internet. |
| Deployment | Docker Compose with Caddy | One command on a Raspberry Pi; local HTTPS with no public certificate authority. |
| Encoding | Canonical CBOR, hand-written subset | Deterministic bytes give deterministic hashes; the subset is small enough to reimplement from SPEC.md. |
| TS tests | Vitest | Fast, no config. |
| Simulator | Python 3 + PyYAML | Scenarios are data, not code, and the engine under test is the real compiled C. |
| Validation and pilot tools | Python 3, standard library | Every rate with its interval, and synthetic input never reported as a measurement (docs/adr/0006). |
| CI | GitHub Actions | Runs the C, TypeScript and Python tests, the golden files, the simulator, the replay and pilot demos and the escalation dry run, and builds the dashboard, the gateway image, the three node images and the two camera images on every push. |

Nothing above costs anything. Total software tooling spend: ₹0. A pilot site's SIM does.

## Conventions

- **TypeScript runs directly.** Node's type stripping executes the sources, so imports
  carry explicit `.ts` extensions and there is no build step in M0. See `docs/adr/0001`.
- **Scores travel as integers.** The ledger encodes 0.62 as `62`; floats are refused by
  the CBOR encoder, because float encodings are the easiest way for two implementations
  to disagree about a hash.
- **Every state change is an event.** If something happened and there is no ledger entry
  for it, that is a bug. What only the gateway sees goes into the site chain.

## Next

The bench is next, in order:

1. The M1 checklist in [docs/bringup](docs/bringup/README.md).
2. Calibration and the M2 demonstration in [m2-bench.md](docs/bringup/m2-bench.md).
3. The M3 demonstration in [m3-bench.md](docs/bringup/m3-bench.md).
4. The M4 sessions in [m4-bench.md](docs/bringup/m4-bench.md).
5. The M5 bench work in [m5-bench.md](docs/bringup/m5-bench.md):
   - the one serial upgrade to two application slots;
   - an update and a rollback;
   - the clock and traces;
   - the cable pull;
   - a drill through a real modem.

Then two pilot sites, run by [docs/pilot.md](docs/pilot.md), with each emergency service
kept in drill mode until it has agreed in writing. The gateway runs on a Raspberry Pi as
described in [infra/README.md](infra/README.md).

6. The M6 bench work in [m6-bench.md](docs/bringup/m6-bench.md):
   - the secure element and the thermistor rehearsed on a dev kit;
   - a production unit through the factory tool;
   - the production locks checked;
   - then the same on the EVT PCB once it is drawn, with its real pin map.

What M6 left open:
- no anti-rollback: an older signed production image can still be offered ([threat model](docs/threat-model.md) §8);
- the TLS key is not yet in the secure element;
- the camera image cannot be updated in the field;
- the security contact for vulnerability reports is not yet published.

After that, Q3 in the [software roadmap](../FireX_Software_Roadmap_v2.pdf):
- NBFLite anchoring through the AnchorProvider, with daily roots and one on every CRITICAL;
- a ten-unit fleet on one gateway, with a fleet health view and staged OTA groups;
- the public verifier page, working offline once loaded;
- media manifests anchored with the log;
- an ICCC or ERSS-112 adapter if a pilot agreement lands.
