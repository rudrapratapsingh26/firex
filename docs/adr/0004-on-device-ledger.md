# ADR 0004 — The on-device ledger

Date: 2026-09-15
Status: accepted for M2-M5; at M6 the key moves on EVT units (see "M6" below), storage unchanged

## Context

M2 puts the FireX ledger on the node: every state change hashed, chained, signed and
stored on flash, verifiable by the TypeScript reference verifier with no FireX
involvement. The node has no network and no clock until M3, one developer, and a dev kit
with no secure element.

## Decisions

**One C implementation, proven against the reference.** `components/ledger/src` is
pure C (SHA-256, canonical CBOR, entries, records, ring buffer, chain writer), built on
the host by `firmware/Makefile`. `ledger/src/golden-c.ts` turns the committed valid-chain
vector into a C header, so the tests compare payload bytes, hashes, the device ID and the
PEM against what the reference produced. CI then exports that chain through the C record,
ring and JSON code and requires `firex-verify` to answer PASS.

**Portable SHA-256, not the hardware accelerator.** The same code runs in the host tests
and on the node. Entry preimages are under a kilobyte and frames are tens of kilobytes;
software SHA-256 on the ESP32-S3 is fast enough for both. The first host run caught a
one-bit error in a round constant, which is the argument for testing the exact code that
ships.

**Segment files on LittleFS, rotating.** Entries are CRC-checked records in 32 KiB
segment files, at most 24 of them on a 1 MiB partition. The oldest segment is removed
when a new one would pass the limit, so the node keeps its most recent few thousand
entries (about 250 bytes each). A record torn by power loss ends its segment; the next
entry starts a new one. A partition that holds data but will not mount is reported and
never formatted.

**Producers never wait.** Alarm, fusion, camera and self-test queue events with a zero
timeout. A full queue raises the ledger fault instead of blocking an alarm path.

**Device key in NVS, deterministic signatures.** A P-256 key is generated on first boot
with the SAR ADC entropy source enabled, before any ADC driver starts, and is checked by
signing and verifying at every boot. Signatures follow RFC 6979, so no per-signature
randomness can repeat a nonce. A stored key that fails validation is an error, never
silently replaced. If the chain on flash was written under a different key the node stops
writing, because extending it would produce entries that fail verification; recovery is
an explicit `ledger wipe ERASE-ALL-EVIDENCE` after an export.

**Uncertain time, never backwards.** `ts_src` is `uncertain` until M3. `ts` starts after
the newest stored entry or at the build time, and advances with uptime.

## Consequences

- An inspector can verify a bench log today with the same verifier the gateway will use.
- History older than the rotation window is lost until M3 mirrors entries to the gateway.
  At bench event rates the window is weeks; in a noisy pilot site it may be days, which is
  why mirroring is the first M3 task.
- Anyone with physical access to a dev-kit node can read its private key over USB and sign
  entries as that node. Accepted for bench and pilots, stated in the threat model, and
  closed by the ATECC608B in M6.
- The daily self-test checks every entry's hash and link but only the newest entry's
  signature: verifying thousands of software ECDSA signatures would hold the ledger for
  minutes. `ledger check sig` on the console verifies all of them.
- Timestamps across a power cut undercount the time the node was off; the `uncertain`
  flag and the boot entry's `reset_reason` make that visible.

## M6

- **Key.** On EVT units the device key is generated in an ATECC608B and never leaves it
  ([ADR 0008](0008-secure-element.md)). Signatures from the chip use randomised nonces
  instead of RFC 6979; they verify identically. Dev-kit units keep the NVS key described
  above.
- **Storage.** Unchanged, except that on the EVT board the ledger partition moves to
  0x400000 and is encrypted in the production image
  ([ADR 0009](0009-secure-boot-and-production-locks.md)).
- **Rules.** The rule that a stored key is never silently replaced becomes a hardware
  property: the key's slot is locked once it is generated.
