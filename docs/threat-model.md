# Threat model

Status: draft, M6. Carries forward the stated limits of Software Architecture v1.0 §06 and
updates them for the EVT board. Nothing here has been reviewed by a third party, and none of the
EVT protections have run on hardware. FireX is **designed against** the practices named in §7;
it is not certified against anything.

An evaluator will trust a threat model that says what it does not protect against more than one
that says "blockchain makes it secure". So this page names what FireX does not do, too.

## 1. What FireX claims

FireX claims that its record of what a unit sensed and did is **tamper-evident**:
- an entry that was changed, removed, reordered or inserted after the unit wrote it is detected
  by a verifier with no FireX involvement;
- a photo whose hash is in the record either matches it or does not.

It does not claim that the unit detects every fire (M4 measures that), that a covered sensor
cannot be fooled, or that the record proves the events were true. The record proves what the unit
wrote, and when it wrote it only as well as its clock allows.

## 2. Assets

| Asset | Why it matters | Where it lives |
|-------|----------------|----------------|
| A unit's evidence chain | The product: what the unit sensed and did | Unit ledger partition; mirrored to the gateway; exported bundles |
| The device key | Whoever can sign with it can write entries as the unit | Dev kit: NVS. EVT: ATECC608B slot 0 |
| The TLS key | Whoever holds it can connect to the broker as the unit | NVS (encrypted on production units) |
| The site CA key | Issues unit and broker certificates, signs the CRL | Gateway data volume |
| The site-ledger key | Signs the site chain: alerts, acknowledgements, gaps, removals | Gateway data volume |
| The release signing key | Decides which firmware runs on every unit | Offline, with its holder |
| Photos and traces | Personal data about people in the room | Unit media partition until acknowledged, then the gateway |
| Pairing codes | Proof of possession when a unit joins a site | Unit NVS, the unit's label, briefly the factory label file |
| Wi-Fi credentials, site certificates | Access to the site network and broker | Unit NVS |
| Factory records | Which device key, firmware and fuses each unit left the factory with | Factory station, `manifest.jsonl` |

## 3. Adversaries

1. **A facility manager** who wants the record of an incident to say something else, with
   dashboard access and physical access to units in the building.
2. **FireX as a vendor**, which writes the firmware, holds the release key and could be asked to
   alter records.
3. **Someone with physical access to a unit**, with a USB cable, a logic analyser, time, and no
   chip-decapping laboratory.
4. **An installer**, who sees pairing codes and adds units to a site.
5. **An attacker on the site LAN**, who can watch and inject traffic.
6. **Someone who steals or copies the gateway** or its backups.
7. **The supply chain and the factory station**: parts, the provisioning PC and its operator.
8. **Accident**: the release key or the site CA is lost.

Out of scope: invasive attacks on the ATECC608B or the ESP32-S3 (decapping, fault injection at a
laboratory), and a compromised build toolchain.

## 4. Protections

| Threat | Protection | Enforced where | Dev kit | EVT production |
|--------|------------|----------------|---------|----------------|
| Editing, deleting or reordering entries after they were written | SHA-256 chain and a per-entry device signature; the verifier names the first bad entry | `components/ledger`, `ledger/src` | Yes | Yes |
| Entries fabricated by the gateway or any other component | Only the device key signs unit entries; the gateway's own events go into a separately signed site chain | Ledger signer; `gateway/server/site-ledger.ts` | Yes | Yes |
| Wiping the log on the unit | The gateway keeps the mirrored chain and opens a new epoch for the restarted one | Gateway ingest (SPEC §7) | Yes, for mirrored entries | Yes, for mirrored entries |
| Extracting the device key | Key generated in the secure element, marked secret, no private write, slot locked | ATECC608B configuration (ADR 0008) | **No**: plain NVS over USB | Yes |
| Reading NVS secrets or photos by dumping flash | Flash encryption (release), NVS encryption, ROM loader in secure download mode | eFuses at first boot (ADR 0009) | No | Yes |
| Running other firmware on the unit | Secure boot v2 with the release key digest in eFuse; JTAG disabled | ROM and bootloader | No | Yes |
| A malicious update | Offline release key; the gateway checks the key digest; the unit checks the signature before switching slots; no confirmation within 60 s rolls back | `tools/ota`, `gateway/server/firmware.ts`, `components/ota` | Yes (M5) | Yes, plus secure boot at every boot |
| A signed development image offered to a production unit, lifting the privacy locks | The image kind (`devkit`, `evt`, `evt-production`) inside the signed image must match the unit's | `main/image_kind.c`, `ota_task.c`, `components/ota` (ADR 0009) | Kinds checked | Yes |
| The camera as a shutter | Capture only on the CRITICAL transition; the camera refuses capture from its console | `camera_task.c`, `cam_main.c` (media policy) | Yes, but bench images and reflashing undo it | Yes; the camera boots only signed firmware, and its loader is disabled |
| Photos leaving by the console | `media get` exists only in non-production images | `console.c` | No | Yes |
| More photos per incident than the policy allows | Frame count fixed at build time; `config set cam_frames` refused | `fx_settings`, `camera_task.c` | No: a setting of 0-5 | Yes |
| A rogue gateway during provisioning | Reply MAC keyed by the pairing code | `components/net`, `gateway/server/pairing.ts` | Yes | Yes |
| Impersonating a unit to the broker | Mutual TLS; per-unit client certificates; access rules by certificate | Mosquitto, `infra/mosquitto/acl` | Yes | Yes |
| A removed unit reconnecting | Certificate serial on the site CRL | Broker (after restart), firmware server | Yes | Yes |
| Guessing a pairing code | 50 bits, single use, 24-hour pairing, failures rate-limited | `gateway/server/provision.ts` | Yes | Yes |
| An unplugged or silent unit | `heartbeat_gap` in the site chain | `gateway/server/heartbeat.ts` | Yes | Yes |
| A covered or failed sensor | Daily self-test entries; invalid never scores zero; flat lines visible on the dashboard | `selftest_task.c`, score API | Partly | Partly |
| Wrong wall-clock time | `ts_src` on every entry; `clock_sync` entries; time from the gateway over the authenticated link | Ledger clock (SPEC §7) | Visible, not prevented | Visible, not prevented |
| A foreign or pre-configured secure element | The firmware refuses a chip whose locked configuration differs from FireX's | `components/secure` (ADR 0008) | n/a | Yes |

## 5. What each adversary can still do

- **Facility manager.** Can unplug a unit, cover a sensor, or wipe the log from the console
  after typing `ERASE-ALL-EVIDENCE`. Each leaves a trace:
  - a heartbeat gap;
  - a flat line;
  - a restarted chain beside the gateway's mirrored copy.

  Entries written while the gateway was unreachable, and wiped before they were mirrored, are
  lost with no copy.
- **FireX.** Cannot re-sign or alter entries already on a gateway, because FireX does not hold
  any device key. But FireX holds the release key. A signed image can ask the secure element to
  sign whatever it likes and can report a false `fw_hash`. The secure element stops the key
  being copied out, not the running firmware from misusing it. Anchoring (Q3) limits what such
  an image could do to history already anchored; until then, this rests on FireX's release
  process and the review trail in the repository.
- **Physical access, dev kit or pilot unit.** Can read the device key, TLS key, pairing code and
  Wi-Fi password over USB and sign entries as the unit. Accepted for bench and pilots.
- **Physical access, EVT production unit.** Cannot read keys or flash contents, run other
  firmware, or use JTAG. Can still:
  - use the console of the running image, which has no command to sign arbitrary data;
  - destroy the unit;
  - watch the I2C bus to the secure element, which carries digests and signatures but no key.
- **Installer.** Can add a unit to the wrong site or read codes from labels. A code works once,
  and every unit added is on the dashboard.
- **LAN attacker.** Can record the provisioning exchange and search the 50-bit code offline,
  which gains nothing once the pairing is used. Can block traffic, which shows as gaps. Cannot
  read MQTT or impersonate a unit without its TLS key.
- **Gateway thief.** Holds the site CA key, the site-ledger key, the database and photos in plain
  form. Can issue certificates and write site-chain entries, but cannot write unit entries. The
  gateway has no hardware key store before Q3 (ADR 0007); backups must be encrypted
  ([operations.md](operations.md)).
- **Factory.** The station never sees a device key: it is generated inside the chip. It does see
  pairing codes while labels are printed; the label file is destroyed afterwards. A station
  running modified software could provision a unit wrongly, which the manifest and
  `security status` at installation expose. It cannot make a unit boot unsigned firmware.
- **Lost release key.** No further updates for any unit. A production unit cannot boot any other
  image either, so there is no recovery short of replacing the board.

## 6. Stated limits

Carried forward from Software Architecture v1.0 §06, updated for M6.

| Protected against | Not protected against, and what covers it instead |
|-------------------|---------------------------------------------------|
| A facility manager editing or deleting log entries after an incident. | A physically tampered or covered sensor. Daily self-tests are logged, and a sensor reading a flat line for days is visible on the dashboard. |
| A vendor (FireX) altering records on the gateway. | A unit that is simply unplugged. The gateway writes a `heartbeat_gap` with its start and end into the site chain. |
| Inserting fabricated entries from any component other than the device. | A key extracted from a **dev-kit or pilot** unit. Accepted for bench and pilots. **Closed on EVT units** by the ATECC608B, where the key is generated in the chip and cannot be read out. |
| Extracting the device key from an EVT unit, or running other firmware on it. | Wrong wall-clock time on an isolated unit. `ts_src` and `clock_sync` entries make it visible rather than silent. |
| Photos leaving a production unit other than to its gateway. | Back-dating a whole record. Chain order and time sources limit it, but only anchoring (Q3) proves a log existed at a given time. |
| | Misuse of the device key by a signed firmware image. The release process and, from Q3, anchoring. |
| A signed development image offered to a production unit. | A signed but older production image offered to a production unit. See §8. |

## 7. TEC 31318:2022

FireX is designed against the Telecommunication Engineering Centre's code of practice for
securing consumer IoT, the document an Indian tender evaluator is most likely to cite. The table
follows its practice areas, which are the same thirteen as ETSI EN 303 645. Check the wording and
clause numbers against the published TEC document before quoting them in a tender.

| Practice area | FireX | Where |
|---------------|-------|-------|
| No universal default passwords | A unique pairing code per unit, generated on the unit, single use. Dashboard accounts are created by the site administrator; there is no default account. | transport.md §1, infra/README.md §3 |
| Manage reports of vulnerabilities | A disclosure contact and process | [operations.md](operations.md) §7 (contact not yet published) |
| Keep software updated | Signed updates over the site LAN, one unit at a time, with rollback | transport.md §6, ADR 0007 |
| Securely store sensitive security parameters | Device key in the ATECC608B; other secrets in encrypted NVS on production units | ADR 0008, ADR 0009 |
| Communicate securely | Mutual TLS for MQTT and firmware download; authenticated provisioning | transport.md |
| Minimise exposed attack surfaces | No inbound listener on the unit except onboarding before pairing; JTAG off; ROM loader restricted; gateway ports documented and closable | infra/README.md, ADR 0009 |
| Ensure software integrity | Secure boot v2 on production units; signature checks before an update | ADR 0009 |
| Ensure that personal data is secure | Media policy: capture only on CRITICAL, photos stay on the site, access logged, retention ends in deletion | media-policy.md |
| Make systems resilient to outages | The alarm never depends on the network; store and forward; heartbeat gaps | architecture.md |
| Examine system telemetry data | Telemetry, self-test entries and heartbeat gaps on the gateway | transport.md §2 |
| Make it easy for users to delete user data | Photos are deleted at the end of retention, and each deletion is logged. Ledger entries are evidence and are not deletable by design; they carry no photos or names. | media-policy.md |
| Make installation and maintenance easy | Pairing code on the label; one Compose stack; documented removal | infra/README.md |
| Validate input data | Canonical CBOR decoder refuses non-canonical input; the gateway verifies every entry and refuses media that does not hash to its topic | ledger/SPEC.md, `gateway/server/ingest.ts` |

## 8. Known gaps, not yet closed

- **No anti-rollback.** The unit refuses only the version it already runs, and no secure version
  is burned into eFuse. An older signed image with a known flaw can be offered by a gateway
  administrator.
- **The TLS key is not in the secure element.** On a production unit it is protected by
  encryption and secure boot, not by hardware (ADR 0008).
- **The broker reads the CRL only at restart** (infra/README.md §9).
- **A compromised release key cannot be rotated out** of production units: unused secure boot
  digest slots are revoked at first boot (ADR 0009).

## 9. Not verified yet

- Every EVT protection above: nothing has run on a board with the secure element, secure boot
  or flash encryption.
- That the ATECC608B configuration in ADR 0008 behaves as its datasheet describes: private key
  unreadable, no private write, slot lock refusing GenKey.
- Secure download mode's refusals on the ESP32-S3, and the disabled ROM loader on the camera.
- NVS encryption key generation on first boot.
