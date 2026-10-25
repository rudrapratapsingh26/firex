# FireX Ledger — specification v0.5

Status: draft, M5. This revision builds on earlier drafts:

- v0.2 superseded the entry format sketched in Software Architecture & Roadmap v1.0 §06 and
  added the five event types introduced in Software Roadmap v2.0.
- v0.3 added the `fault` event and node implementation notes.
- v0.4 added the `log_overwrite` event and gateway mirroring notes.
- v0.5 is new in this revision:
  - the `gateway` time source;
  - the `fw_update` and `trace_captured` unit events, and a defined `clock_sync` payload;
  - the **site chain**, a second chain in the same format written by the site gateway.

Nothing in v0.5 changes how an entry is encoded, hashed or signed: every earlier log still
verifies, and the same verifier checks a site chain unchanged.

M6 changed only the implementation notes in §7, for signatures and the device key on EVT units.
Nothing normative changed, and the version stays v0.5.

A FireX node writes every state change to an append-only log. Each entry carries the
hash of the one before it and is signed by the device, so altering one entry breaks
every entry after it. An inspector verifies a log with no account, no server and no
involvement from FireX.

---

## 1. Entry

Entries are encoded as canonical CBOR (RFC 8949 §4.2). Field order in the map is not
significant — canonical encoding sorts keys — but every field is mandatory.

| Field       | CBOR type      | Notes                                                      |
|-------------|----------------|------------------------------------------------------------|
| `seq`       | uint           | Monotonic per device, starting at 0 for the genesis entry.  |
| `ts`        | uint           | Unix seconds.                                               |
| `ts_src`    | tstr           | `ntp` \| `rtc` \| `gateway` \| `uncertain` — how the clock was known (§7). |
| `device_id` | bstr(16)       | First 16 bytes of SHA-256 over the device public key (SPKI).|
| `type`      | tstr           | Event type, §3.                                             |
| `payload`   | map            | Event-specific, §3. May be empty.                           |
| `fw_hash`   | bstr(32)       | SHA-256 of the running firmware image (for a site chain, §3.5). |
| `prev_hash` | bstr(32)       | `hash` of entry `seq-1`; 32 zero bytes for the genesis.     |
| `hash`      | bstr(32)       | §2.                                                         |
| `sig`       | bstr(64)       | §2.                                                         |

## 2. Hash and signature

Let `body` be the canonical CBOR encoding of the map containing exactly
`seq`, `ts`, `ts_src`, `device_id`, `type`, `payload`, `fw_hash` and `prev_hash`
— that is, every field except `hash` and `sig`.

```
preimage = prev_hash || body            (32 bytes, then the CBOR body)
hash     = SHA-256(preimage)
sig      = ECDSA-P256-SHA256(preimage)  signed with the device key, raw r||s, 64 bytes
```

Signing the preimage rather than the hash keeps one definition for both the device
(mbedTLS computes the digest, then signs it) and the verifier (`crypto.sign('sha256', …)`
in Node, `dsaEncoding: 'ieee-p1363'`). Verifying a log means checking, for every entry:

1. **Sequence** — `seq` increases by exactly 1 with no gaps.
2. **Chain** — `prev_hash` equals the previous entry's `hash`, and `hash` equals
   `SHA-256(preimage)` recomputed from the entry's own fields.
3. **Signature** — `sig` verifies over `preimage` against the device public key.

The verifier reports the **first** entry that fails and which of the three tiers failed.

## 3. Event types

Types from v1.0, unchanged:

| Type              | Payload                                                    |
|-------------------|------------------------------------------------------------|
| `boot`            | `fw_version`, `reset_reason`, `boot_count`                  |
| `self_test`       | `result` (`pass`/`fail`), `checks` (map of name → bool)      |
| `threshold_cross` | `modality`, `score`, `direction` (`up`/`down`)               |
| `state_change`    | `from`, `to`, `reason` (array of modalities), `scores`       |
| `config_change`   | `key`, `old`, `new`                                          |
| `calibration`     | `modality`, `r0` or `baseline`, `method`                     |
| `silence`         | `seconds`, `by` (`button`)                                   |
| `heartbeat_gap`   | written in the site chain from v0.5, §3.5                    |
| `clock_sync`      | payload defined in v0.5, below                              |
| `anchor_ack`      | `period`, `root` (bstr 32), `provider`, `receipt`             |

### 3.1 New in v0.2

| Type             | Payload                                                                     | Why it exists |
|------------------|------------------------------------------------------------------------------|---------------|
| `media_captured` | `trigger` (state), `frames` (array of bstr(32) SHA-256), `clip` (bstr(32) or null), `codec`, `resolution` | Makes a photo evidence: re-hash the file and it either matches the chain or it does not. |
| `alert_sent`     | `service` (`fire`\|`police`\|`ambulance`\|`site`), `channel` (`api`\|`sms`\|`voice`), `recipient`, `receipt_id` | Proves the alert left the building, and when. Written in the site chain from v0.5, with the fields in §3.5. |
| `alert_ack`      | `service`, `receipt_id`, `by`, `latency_s`                                    | Turns response time into a measured number instead of a claim. Site chain from v0.5. |
| `alert_timeout`  | `service`, `receipt_id`, `elapsed_s`, `action` (`re_alert`\|`escalate`)       | Records silence, which matters more than success in an inquiry. Site chain from v0.5. |
| `fan_state`      | `state` (`on`\|`off`\|`failed`), `duty`, and `current_ma` or `rpm` (whichever the hardware measures; neither if it measures nothing) | Response time depends on airflow, so a stopped fan must be visible in the record. |

### 3.2 New in v0.3

| Type    | Payload                                                  | Why it exists |
|---------|----------------------------------------------------------|---------------|
| `fault` | `source` (a modality — `gas`, `smoke`, `flame`, `heat`, `humidity` — or a subsystem), `state` (`raised`\|`cleared`) | A sensor that stops reporting is excluded from voting; the record must show when the unit was deciding on fewer signals. |

In v0.3 `media_captured.frames` may be an empty array: a capture was attempted on
CRITICAL and produced nothing (camera silent, flash full). `clip` is `null` when no clip
was recorded. `calibration.method` is `clean_air_median` for a measured baseline and
`manual` for a value entered by hand.

### 3.3 New in v0.4

| Type            | Payload                                                  | Why it exists |
|-----------------|----------------------------------------------------------|---------------|
| `log_overwrite` | `first_seq`, `last_seq` (the range the node's storage removed), `mirrored` (bool: the gateway had acknowledged every entry in the range) | Rotation on the node destroys evidence. Recording it in a signed entry means an export that starts after a gap, or a gateway mirror with a hole, carries its own explanation. |

The node writes `log_overwrite` as the entry after the one whose storage caused the
removal. `mirrored: true` means the removed entries still exist on the gateway, so the
site's record is complete. `mirrored: false` marks a real loss: the unit was cut off from
its gateway for longer than its storage holds.

Images themselves never enter the ledger — only their hashes. The log can therefore be
published, exported and anchored without publishing the pictures.

### 3.4 New in v0.5: unit entries

| Type             | Payload | Why it exists |
|------------------|---------|---------------|
| `fw_update`      | `phase` (`accepted`\|`downloaded`\|`confirmed`\|`failed`\|`rolled_back`), `version` (the image this phase is about), `from_version`, `image` (bstr(32), SHA-256 of the image file; 32 zero bytes if unknown), `reason` (tstr, empty when there is none) | The firmware an alarm ran on is evidence. A signed update history shows which image made each decision, and that a failed one was taken back out. |
| `trace_captured` | `trigger` (`ELEVATED`\|`CRITICAL`), `trace` (bstr(32), SHA-256 of the trace file), `samples`, `period_ms`, `pre_samples` | The raw scores around an episode, as a file whose hash is in the chain, so a replay of the pilot's data starts from evidence (file format: `docs/transport.md` §7). |
| `clock_sync`     | `source` (`gateway`), `step_ms` (uint), `direction` (`forward`\|`back`) | The moment a unit's timestamps start meaning wall-clock time, and how far its clock moved. The v1.0 names (`offset_ms`) were never written; the CBOR subset has no negative integers, hence a magnitude and a direction. |

A unit writes `fw_update` `accepted` when it takes an update command, and `failed` with a
`reason` beginning `refused:` when it will not (not in NORMAL, the same version, a malformed
command). It writes `downloaded` once the image is stored and its hash and signature check. The new image writes `confirmed`
after it passes its start-up checks. If it does not pass them in time, it writes `failed`
before the bootloader returns to the previous image, if it can. The previous image then
writes `rolled_back` once. Its `version` names the image that failed.

### 3.5 New in v0.5: the site chain

A unit's chain is signed by the unit, and nothing else can extend it. Some events are
witnessed only by the site gateway: alerts sent and answered, a unit falling silent, a
site contact labelling an incident, drills, changes to who is called. The gateway keeps
them in its own chain. It uses exactly the entry format of §1 and §2, so the same verifier
checks it, and it differs from a unit's chain only in these ways:

- `device_id` is derived from the gateway's site-ledger public key (P-256, generated on the
  gateway at first start).
- `fw_hash` is SHA-256 over the UTF-8 string `firex-server/<gateway version>`.
- `ts_src` is `ntp` when the gateway's clock is known to be synchronised, otherwise
  `uncertain`. The gateway never writes a `ts` below the previous entry's.
- An exported site chain is a bundle with `"kind": "site"`, verified like any other.

Entries about a unit name it by `unit` (its `device_id` in hex). Entries about an incident
name the unit entry that opened it by `critical_seq` and `critical_hash`, which ties the
two chains together without either signing the other's data.

| Type | Payload | Why it exists |
|------|---------|---------------|
| `alert_sent` | `service`, `channel` (`api`\|`sms`\|`voice`\|`console`\|`dashboard`), `recipient`, `receipt_id`, `incident_id` (tstr), `unit`, `incident` (uint, the gateway's incident number; 0 for a drill), `critical_seq`, `critical_hash` (bstr(32); zeros for a drill), `drill` (bool), and `retry_of` on a re-alert | Who was told, on which channel, about which recorded alarm, and whether it was a drill. |
| `alert_ack` | `service`, `receipt_id`, `by`, `latency_s`, `unit`, `incident`, `drill` | Who answered and how long it took. |
| `alert_timeout` | `service`, `receipt_id`, `elapsed_s`, `action` (`re_alert`\|`escalate`), `unit`, `incident`, `drill` | Silence, recorded. |
| `escalation_withheld` | `unit`, `incident`, `critical_seq`, `reason` (`verification`\|`late`\|`drill_mode`) | Why a recorded CRITICAL did not reach every service: its entry failed verification, it arrived late after an outage, or the services are not yet live. |
| `heartbeat_gap` | `unit`, `start_ts`, `end_ts`, `seconds` | A unit the gateway did not hear from. The unit's own chain says what it did meanwhile; this says the site could not see it. |
| `incident_label` | `unit`, `incident`, `critical_seq`, `critical_hash`, `label` (`true`\|`nuisance`\|`test`), `by` | The site contact's judgement of an alarm, with the account that made it. |
| `drill` | `drill_id`, `by`, `services` (array of tstr), `phase` (`started`\|`finished`) | A test of the escalation path, kept apart from real alarms. |
| `registry_change` | `old_registry` (bstr(32) or null), `new_registry` (bstr(32)), `by` | Who was supposed to be called, answerable months later: SHA-256 of the registry file before and after. |
| `fw_offered` | `unit`, `version`, `image` (bstr(32)), `by` | Which firmware the site sent to which unit, and on whose instruction. |
| `unit_removed` | `unit`, `by`, `cert_serial` | A unit taken off the site and its certificate revoked. |

## 4. Canonical CBOR rules used here

A deliberately small subset, so that a third-party implementation can match it byte for byte:

- Integers use the shortest form; no negative integers appear in any entry.
- Text and byte strings use definite lengths; no indefinite-length items.
- Maps use definite lengths and their keys are sorted by the **encoded bytes** of the key.
- Floats are avoided: scores are encoded as integers in hundredths (`0.62` → `62`).
- `null` is used only where a field is explicitly nullable (for example `clip`).

In the JSON transport, byte strings are lowercase hex. A verifier that rebuilds the CBOR
from JSON must know which payload fields are byte strings: `frames` (array), `clip`, `root`,
and from v0.5 `image`, `trace`, `critical_hash`, `old_registry` and `new_registry`.

## 5. Anchoring

The gateway builds a Merkle tree over the entries of every node for a period, daily and
on any CRITICAL, and sends the 32-byte root to the anchor via the `AnchorProvider`
interface. Only the root leaves the building — no telemetry, no logs, no PII, no images.
From v0.5 the site chain is anchored with the unit chains.

## 6. Test vectors

`ledger/vectors/` holds ten vectors generated by `npm run vectors`: one valid chain and
nine broken ones, each naming the entry and tier that must fail. A verifier that agrees
with all ten is interoperable with this spec by definition. The v0.5 event types do not
change encoding, so the vectors are unchanged; `gateway/test/site-ledger.test.ts` checks a
site chain against the same verifier.

| Vector | Expect |
|--------|--------|
| `01-valid-chain` | PASS |
| `02-tampered-payload` | TAMPERED, chain, entry 3 |
| `03-broken-prev-link` | TAMPERED, chain, entry 4 |
| `04-bad-signature` | TAMPERED, signature, entry 2 |
| `05-sequence-gap` | TAMPERED, sequence, entry 4 |
| `06-reordered-entries` | TAMPERED, sequence, entry 2 |
| `07-truncated-middle` | TAMPERED, sequence, entry 3 |
| `08-wrong-device-id` | TAMPERED, signature, entry 2 |
| `09-media-hash-mismatch` | TAMPERED, media, entry 5 |
| `10-genesis-prev-not-zero` | TAMPERED, chain, entry 0 |

## 7. Node and gateway implementation notes (v0.5)

These describe the FireX node firmware and the site gateway. They constrain nothing a
verifier does.

- **Time.** The node has no battery-backed clock. Until its gateway sends the time on a
  boot, entries carry `ts_src` = `uncertain`. `ts` starts one second after the newest
  entry already on flash (or at the firmware build time for a fresh log) and advances with
  uptime. Once the gateway sends its time over the authenticated MQTT link
  (`docs/transport.md` §5), entries carry `ts_src` = `gateway` for the rest of that boot.
  The first sync of a boot, and any later correction of 2 s or more, writes `clock_sync`,
  itself stamped with the new time; smaller corrections are applied without an entry.
  `ts` never runs backwards within a chain: if the gateway's time is behind the last
  written `ts`, entries keep that `ts` until real time passes it. `gateway` time is only as
  good as the gateway's own clock, which the site chain's `ts_src` states.
- **Storage.** Entries are stored as CRC-checked records in numbered segment files on a
  dedicated LittleFS partition. When the partition's segment budget is reached, the oldest
  segment is removed. An export may therefore begin at `seq` > 0; the reference verifier
  accepts that first entry's `prev_hash` as the starting point. Every removal is recorded
  as a `log_overwrite` entry (v0.4).
- **Mirroring (M3).** The node publishes each entry to the site gateway as it is written,
  and again from the gateway's last acknowledgement after any outage or reboot
  (`docs/transport.md`). Mirroring never delays rotation: a unit offline for longer than
  its storage holds still loses entries, and `log_overwrite` says whether it did.
- **Epochs on the gateway.** When a unit's chain restarts below what the gateway already
  holds (its log was wiped), the gateway keeps the old chain and starts a new epoch. Each
  epoch is exported and verified on its own.
- **Verdicts on the gateway.** The gateway checks every entry's link, hash and signature
  on arrival and shows the result on the dashboard. That verdict is a convenience: an
  inspector re-verifies the exported bundle with the reference verifier.
- **Signatures.** ECDSA P-256 over SHA-256 of the preimage. On a dev-kit node they are
  produced deterministically (RFC 6979) by mbedTLS. On an EVT node (M6) the secure element
  signs, with a nonce from its own random number generator. The gateway's site chain uses
  Node's randomised signatures. Deterministic and randomised signatures verify identically,
  and a verifier cannot and need not tell them apart.
- **Device key.** On the dev kit it is generated on the node at first boot from the
  hardware RNG and stored in NVS in plain form (an accepted risk for bench and pilot units).
  On an EVT node (M6) it is generated inside an ATECC608B, whose slot is then locked, and
  the private key never leaves the chip (`docs/adr/0008-secure-element.md`). The
  `device_id` is derived from the public key the same way on both. If the node finds a chain
  on flash written under a different key it stops writing rather than extend that chain with
  signatures that would fail. A firmware update keeps the key: neither NVS nor the secure
  element is part of an application slot.
- **Site-ledger key.** Generated on the gateway at first start and kept in its data volume
  in plain form, beside the site CA. Losing it ends the chain; a new key starts a new site
  chain, and the old one still verifies under the old public key.
