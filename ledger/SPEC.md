# FireX Ledger — specification v0.2

Status: draft, M0. Supersedes the entry format sketched in Software Architecture &
Roadmap v1.0 §06 and adds the five event types introduced in Software Roadmap v2.0.

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
| `ts_src`    | tstr           | `ntp` \| `rtc` \| `uncertain` — how the clock was known.    |
| `device_id` | bstr(16)       | First 16 bytes of SHA-256 over the device public key (SPKI).|
| `type`      | tstr           | Event type, §3.                                             |
| `payload`   | map            | Event-specific, §3. May be empty.                           |
| `fw_hash`   | bstr(32)       | SHA-256 of the running firmware image.                      |
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
| `heartbeat_gap`   | `start_ts`, `end_ts`, `seconds` (written by the gateway)      |
| `clock_sync`      | `offset_ms`, `source`                                        |
| `anchor_ack`      | `period`, `root` (bstr 32), `provider`, `receipt`             |

### New in v0.2

| Type             | Payload                                                                     | Why it exists |
|------------------|------------------------------------------------------------------------------|---------------|
| `media_captured` | `trigger` (state), `frames` (array of bstr(32) SHA-256), `clip` (bstr(32) or null), `codec`, `resolution` | Makes a photo evidence: re-hash the file and it either matches the chain or it does not. |
| `alert_sent`     | `service` (`fire`\|`police`\|`ambulance`\|`site`), `channel` (`api`\|`sms`\|`voice`), `recipient`, `receipt_id` | Proves the alert left the building, and when. |
| `alert_ack`      | `service`, `receipt_id`, `by`, `latency_s`                                    | Turns response time into a measured number instead of a claim. |
| `alert_timeout`  | `service`, `receipt_id`, `elapsed_s`, `action` (`re_alert`\|`escalate`)       | Records silence, which matters more than success in an inquiry. |
| `fan_state`      | `state` (`on`\|`off`\|`failed`), `duty`, `current_ma`                          | Response time depends on airflow, so a stopped fan must be visible in the record. |

Images themselves never enter the ledger — only their hashes. The log can therefore be
published, exported and anchored without publishing the pictures.

## 4. Canonical CBOR rules used here

A deliberately small subset, so that a third-party implementation can match it byte for byte:

- Integers use the shortest form; no negative integers appear in any entry.
- Text and byte strings use definite lengths; no indefinite-length items.
- Maps use definite lengths and their keys are sorted by the **encoded bytes** of the key.
- Floats are avoided: scores are encoded as integers in hundredths (`0.62` → `62`).
- `null` is used only where a field is explicitly nullable (for example `clip`).

## 5. Anchoring

The gateway builds a Merkle tree over the entries of every node for a period, daily and
on any CRITICAL, and sends the 32-byte root to the anchor via the `AnchorProvider`
interface. Only the root leaves the building — no telemetry, no logs, no PII, no images.

## 6. Test vectors

`ledger/vectors/` holds ten vectors generated by `npm run vectors`: one valid chain and
nine broken ones, each naming the entry and tier that must fail. A verifier that agrees
with all ten is interoperable with this spec by definition.

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
