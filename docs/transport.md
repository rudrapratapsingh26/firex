# Node ↔ gateway transport (M3, extended in M5)

Status: draft, M5. The contract `firmware/components/net` and `gateway/server` both implement.
Nothing here changes a ledger entry: entries travel as the same JSON the verifier reads.
M5 added the clock (§5), firmware updates (§6) and episode traces (§7).

## 1. Provisioning

A unit ships with a **pairing code** printed on its label: ten Crockford base32 characters,
shown as `XXXXX-XXXXX` (50 bits). The node generates it on first boot and keeps it in NVS.
The same code is the proof-of-possession for Wi-Fi onboarding.

1. The installer enters the code in the dashboard with the site, a location label and a
   position on the floor plan. The gateway keeps it as a pairing that expires in 24 hours
   and can be used once.
2. The installer gives the unit the site network and the gateway's address, either through
   the unit's onboarding access point (ESP SoftAP provisioning, security 1 with the pairing
   code as proof of possession, the address sent as `host[:port]` on the `custom-data`
   endpoint) or on its USB console (`wifi set`, `gateway set`). The node, once on the site
   Wi-Fi, sends `POST /api/provision` to the gateway's provisioning port (plain HTTP on the
   LAN, 8081 by default):

   ```json
   {
     "version": 1,
     "device_id": "<32 hex: ledger device_id>",
     "ledger_pub": "<130 hex: uncompressed P-256 point of the ledger key>",
     "tls_pub": "<130 hex: uncompressed P-256 point of a separate TLS key>",
     "fw_version": "0.4.0-m3",
     "nonce": "<32 hex, random>",
     "ledger_sig": "<128 hex>",
     "tls_sig": "<128 hex>",
     "proof": "<64 hex>"
   }
   ```

   with `M` = `"firex-provision-v1\n" + device_id + "\n" + ledger_pub + "\n" + tls_pub + "\n" + nonce`,
   `proof` = HMAC-SHA256(key = the normalised code, `M`), and each signature ECDSA-P256-SHA256
   over `M` in raw `r || s` form. The signatures prove the node holds both private keys; the
   proof shows it holds the code. On an EVT unit (M6) `ledger_sig` comes from the ATECC608B,
   which holds the ledger key (ADR 0008), and `tls_sig` from the TLS key in NVS. Nothing on
   the wire changes, and the gateway checks both the same way.
3. The gateway checks that `device_id` is derived from `ledger_pub`, both signatures, and the
   proof against every open pairing. On a match it issues a client certificate for `tls_pub`
   (CN = `device_id`, client authentication only) signed by the site CA, registers the device,
   and marks the pairing used:

   ```json
   {
     "version": 1,
     "site_id": "site-001",
     "device_id": "…",
     "mqtt_host": "firex-gateway.local",
     "mqtt_port": 8883,
     "ca_pem": "…",
     "cert_pem": "…",
     "mac": "<64 hex>"
   }
   ```

   `mac` = HMAC-SHA256(code, `"firex-provision-v1-reply\n" + nonce + "\n" + site_id + "\n" +
   mqtt_host + "\n" + mqtt_port + "\n" + sha256hex(ca_pem) + "\n" + sha256hex(cert_pem)`).
4. The node accepts the reply only if `mac` verifies, so a device on the LAN that does not know
   the code cannot hand it a rogue CA. It stores the CA and certificate and connects to MQTT.

Errors: `400` malformed, `403` no open pairing matches, `409` the device is registered with a
different ledger key, `429` too many failed attempts.

Limits, stated: the provisioning exchange is authenticated, not encrypted. Someone who records
it can try all 2^50 codes offline, which gains nothing once the pairing is used. The window is
the pairing's lifetime; failed attempts are rate-limited.

**Removal (M5).** Removing a unit on the gateway revokes its certificate: the serial goes
into the site CA's revocation list, which the broker and the firmware download server check,
and the gateway ignores anything still published under the device ID. The unit can join the
site again only with a new pairing.

## 2. MQTT

MQTT 3.1.1 over TLS 1.2+, mutual authentication: the node presents its certificate, the broker
presents one signed by the same site CA. The broker's access list lets a client with CN `X`
publish only under `firex/+/X/` and subscribe only to `firex/+/X/cmd`.

Topics under `firex/<site_id>/<device_id>/`:

| Topic | Direction | QoS | Payload |
|-------|-----------|-----|---------|
| `hello` | node → gateway | 1 | `{"fw_version","boot_count","next_seq","oldest_seq","uptime_ms","clock","ota"}` on every connect; `clock` is `gateway` or `uncertain`, `ota` is `{"partition":"ota_0"\|"ota_1","pending_verify":bool}` (M5) |
| `state` | node → gateway | 1, retained | `{"fusion","alarm","faults":{"sensor","ledger","selftest","buzzer"},"uptime_ms"}` on change and every 60 s |
| `telemetry` | node → gateway | 0 | `{"uptime_ms","scores":{"gas":62,…},"fan":{"state","duty","rpm"}}` every 10 s; scores in hundredths, invalid modalities left out |
| `ledger` | node → gateway | 1 | one EntryJson (as `firex-verify` reads it) per message, `seq` strictly increasing |
| `media/<sha256>` | node → gateway | 1 | the raw JPEG whose SHA-256 is the last topic level |
| `trace/<sha256>` | node → gateway | 1 | a trace file (§7) whose SHA-256 is the last topic level, at most 1 MiB (M5) |
| `cmd` | gateway → node | 1 | see below |

Commands:

| `cmd` | Fields | Meaning |
|-------|--------|---------|
| `sync` | `have_seq` (−1 for none) | Reply to `hello`: the highest `seq` the gateway holds for this chain. The node sends from `have_seq + 1`, or its oldest entry if that is later. |
| `ack` | `seq` | Everything up to `seq` is stored. |
| `resend` | `from_seq` | The gateway saw a gap; send again from here. |
| `media_ack` | `sha256` | The image is stored and its hash checked; the node may delete its copy. |
| `time` | `unix_ms` | The gateway's clock (§5). Sent after every `sync` and every hour while connected. (M5) |
| `ota` | `version`, `size`, `sha256`, `url` | Update to this image (§6). (M5) |
| `trace_ack` | `sha256` | The trace file is stored and its hash checked; the node may delete its copy. (M5) |

A node ignores a command it does not know, so a newer gateway can talk to an older unit.

## 3. Store and forward

The node's ledger on flash **is** the outbound queue. Entries are never deleted because they
were sent, only rotated out when the partition fills. On reconnect the `hello`/`sync` exchange
finds the first entry the gateway lacks, whatever happened in between: a Wi-Fi outage, a
reboot, a gateway restart. Media files and trace files wait on the node's media partition
until `media_ack` or `trace_ack`.

If the ledger rotates out entries the gateway never received, `hello.oldest_seq` is higher
than `sync.have_seq + 1`. The gateway records the gap, accepts the node's oldest entry as a new
starting point (its signature still verifies; its link to the missing entry cannot), and marks
the device's chain as having a gap. The node writes a `log_overwrite` entry when it rotates
unmirrored entries, so the loss is also in the signed record.

If the node's `next_seq` is at or below what the gateway holds, the node's chain has restarted
(its ledger was wiped). The gateway opens a new **epoch** for the device and keeps the old one.

**Backlog (M5).** The gateway keeps the `next_seq` of each unit's latest `hello`. Until it
holds every entry up to that point, the unit is catching up. A CRITICAL that arrives in a
backlog, and has already ended in that backlog, was an alarm the site could not see at the
time. The escalation service treats it as late (`docs/service-registry.md`).

## 4. What the gateway does with each entry

On receipt, before acknowledging: the sequence must follow the last stored entry; `prev_hash`
must equal that entry's `hash`; the hash is recomputed; the signature is checked against the
ledger key registered at provisioning. The entry is stored with the result (`ok`, `start`,
`gap`, `chain`, `signature`). A `chain` or `signature` result marks the device's chain broken
and raises it on the dashboard. Entries are never rejected for failing verification: an
inspector needs to see the tampered record, not its absence.

## 5. Clock (M5)

The node has no battery-backed clock. The gateway sends `{"cmd":"time","unix_ms":N}` after
every `sync` and every hour while the unit is connected. The node takes that time for its
ledger clock and marks entries `ts_src` = `gateway` for the rest of the boot. The first sync
of a boot, and any later step of 2 s or more, writes a `clock_sync` entry. The node never
writes a `ts` below one it has already written: if the gateway's time is behind, the unit
holds its `ts` until real time catches up (ledger SPEC §7).

The time travels over the mutually authenticated MQTT link, not unauthenticated SNTP. Its
error is the MQTT delay, tens of milliseconds on a site LAN, well inside the one-second
resolution of `ts`.

## 6. Firmware updates (M5)

Images are ESP-IDF application images with a secure boot v2 signature block appended. They
are signed offline with the FireX release key (`tools/ota/`); the gateway never holds the key.

1. An administrator adds the image to the gateway. The gateway reads its version and
   project name, and checks that the signature block is present and names the expected
   key.
2. A rollout offers the image to one unit at a time with
   `{"cmd":"ota","version":"0.6.0","size":N,"sha256":"…","url":"https://<gateway>:8443/fw/<sha256>.bin"}`.
   Each offer is an `fw_offered` entry in the site chain.
3. The unit accepts only in NORMAL, outside warm-up, with no self-test running, from an
   image that has itself been confirmed, and for a version other than the one it runs. A
   refusal is written as `fw_update` `failed` with a reason beginning `refused:`, so the
   rollout sees it at once. It writes `fw_update` `accepted` and downloads the
   image at its lowest task priority. The download is HTTPS with mutual TLS: the gateway
   presents its broker certificate and the unit its own client certificate, which the
   gateway checks against the site CA and its list of units not removed.
4. The unit checks the size, the SHA-256 and the signature, writes the image to its other
   application slot and writes `fw_update` `downloaded`. It restarts into the new image
   only after 60 s in NORMAL with no incident open.
5. The new image must confirm within 60 s of starting:
   - the ledger is open and its `boot` entry is stored;
   - the alarm and fusion tasks are running;
   - every sensor the previous image had ready is ready;
   - the unit has reconnected to the gateway and synced.

   If so, it writes `fw_update` `confirmed`. If not, it writes `fw_update` `failed` if it
   can, and the bootloader returns to the previous image, which writes `fw_update`
   `rolled_back` once.
6. The gateway follows each unit through its `fw_update` entries. It moves to the next unit
   only after `confirmed`, and stops the rollout on `failed`, on `rolled_back`, or after 15
   minutes without an answer.

After a restart that kept power on the MQ heaters (software, watchdog, panic, reset
button), warm-up is `fx_warm_s` (30 s by default) instead of `fx_warmup_s`, so an update
does not blind a unit for three minutes.

## 7. Episode traces (M5)

A trace is the raw input to fusion around an episode, so pilot data can be replayed through
the M4 harness. It is not telemetry: it exists only when fusion left NORMAL.

**When.** The node keeps the last 5 minutes of samples at 500 ms in memory. When fusion
leaves NORMAL for ELEVATED or CRITICAL, a trace starts with that pre-window. It continues
until fusion has been NORMAL for 2 minutes, for at most 30 minutes in all. A CRITICAL
inside an ELEVATED episode is part of the same trace. The file is written to the media
partition as `t<boot>-<n>.fxt` while being hashed. When it closes, the node writes
`trace_captured`, publishes it on `trace/<sha256>`, and deletes it after `trace_ack` once
it has re-hashed its copy. Photos come first: traces are published only while no photo is
waiting. A trace starts only if a full 30-minute file fits and 512 KiB stay free for
photos; a trace that would eat into that reserve is abandoned, and the node logs it. An
episode still alarming at the 30-minute cap continues in a new file, whose pre-window
overlaps the end of the previous one.

**Format.** Little-endian.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | `"FXTR"` |
| 4 | 1 | format version, 1 |
| 5 | 1 | modalities, 5 |
| 6 | 2 | `period_ms` |
| 8 | 4 | `pre_samples`: samples before the trigger |
| 12 | 4 | `samples` |
| 16 | 1 | trigger state (`fx_state_t`: 2 ELEVATED, 3 CRITICAL) |
| 17 | 3 | zero |
| 20 | 4 | `trigger_uptime_ms`: the unit's uptime at the trigger |
| 24 | 4 | `boot_count` |
| 28 | 4 | zero |
| 32 | 16 × `samples` | samples |

Each sample:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 2 × 5 | scores in thousandths (0-1000; `0xFFFF` invalid): gas, smoke, flame, heat, humidity, as fed to fusion (before humidity suppression) |
| 10 | 1 | fusion state (`fx_state_t`) |
| 11 | 1 | alarm state (`fx_alarm_state_t`) |
| 12 | 1 | fan duty, % |
| 13 | 1 | fan state: 0 off, 1 on, 2 failed, 255 absent |
| 14 | 2 | zero |

Sample `i` was taken at uptime `trigger_uptime_ms + (i − pre_samples) × period_ms`. The
gateway converts traces into the M4 capture format, with trial marks from the site's
labels (`docs/pilot.md`). The golden file `firmware/test/trace_golden.fxt` is the byte-level
reference both sides test against.
