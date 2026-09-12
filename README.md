# FireX

A multi-sensor hazard-detection node: five hazard signals on one device, a critical alarm
only when two independent signals agree, a camera that captures the room when that
happens, alerts to the fire station, police station and ambulance that cover the site, and
every event written to a tamper-evident record a public buyer can verify.

**Stage: TRL 2.** This repository is M0 of the software roadmap — the work that needs a
laptop and a ₹2,200 sensor kit, and no hardware, funding or incorporation. Nothing here
has run on a board yet.

## What works today

```bash
npm install                 # dev dependencies only; nothing ships at runtime
npm test                    # 27 TypeScript tests: ledger, verifier, escalation
make -C firmware test       # 19 Unity tests for the fusion engine
make -C firmware sim        # build the engine harness the simulator drives
python tools/simulator/simulate.py     # four scenarios through the real C engine
npm run demo                # the M0 milestone, below
npm run demo:escalation     # one alarm, four recipients, one silent service
npm run vectors             # regenerate ledger/vectors/
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
firmware/
  components/fusion/    pure C11 engine: five scores in, alarm states out (no hardware deps)
  sim/fusion_cli.c      line protocol around the engine, for the simulator
  test/                 Unity host tests
ledger/
  SPEC.md               entry format, hash and signature construction, event types
  src/                  canonical CBOR, entry signing, verifier, CLI, vector generator
  vectors/              ten test vectors: one valid chain, nine broken
gateway/
  escalation/           AlertProvider interface, ConsoleAlert, the escalation service
tools/simulator/        YAML scenarios driven through the real engine
docs/                   media policy, service registry, architecture decisions
```

## Stack

| Layer | Choice | Why |
|-------|--------|-----|
| Fusion engine | C11, no dependencies | The same code runs on the node and in CI; it has no hardware includes, so it compiles anywhere. |
| Firmware (M1+) | ESP-IDF on FreeRTOS, ESP32-S3 | Signed OTA, A/B partitions, secure boot, hardware SHA-256. Not Arduino. |
| Host tests (C) | Unity | Vendored in `firmware/test/unity`; Ceedling arrives with the ESP-IDF project in M1. |
| Ledger, verifier, gateway | TypeScript on Node 22 | One language across gateway, dashboard and verifier for a one-person team. |
| Crypto | `node:crypto` (SHA-256, ECDSA P-256) | No runtime dependencies at all: an evaluator reads the verifier without auditing a dependency tree. On the node the same primitives come from mbedTLS. |
| Encoding | Canonical CBOR, hand-written subset | Deterministic bytes give deterministic hashes; the subset is small enough to reimplement from SPEC.md. |
| TS tests | Vitest | Fast, no config. |
| Simulator | Python 3 + PyYAML | Scenarios are data, not code, and the engine under test is the real compiled C. |
| CI | GitHub Actions | Builds the firmware tests, runs both suites and the simulator on every push. |

Nothing above costs anything. Total software tooling spend: ₹0.

## Conventions

- **TypeScript runs directly.** Node's type stripping executes the sources, so imports
  carry explicit `.ts` extensions and there is no build step in M0. See `docs/adr/0001`.
- **Scores travel as integers.** The ledger encodes 0.62 as `62`; floats are refused by
  the CBOR encoder, because float encodings are the easiest way for two implementations
  to disagree about a hash.
- **Every state change is an event.** If something happened and there is no ledger entry
  for it, that is a bug.

## Next

M1 brings up the dev kit: one driver per sensor behind the score API, the DHT22 in place
of the BME280 and DS18B20, fan control, and a first JPEG out of the ESP32-CAM. See the
[software roadmap](../FireX_Software_Roadmap_v2.pdf).
