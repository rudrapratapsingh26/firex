# M5 bench: updates, clock, traces and live escalation

Status: written before any of it has run on hardware. The gateway, escalation, export and
report run on a laptop against simulated units and a fake modem. The node firmware compiles
and a throwaway-key signed image verifies with `espsecure.py`, but nothing has run on a
board.

M5 milestone (Software Roadmap v2.0): **pilot data from two live sites and a logged escalation
drill, showing who was alerted, who acknowledged, and how long each took.** This guide is
the bench work that has to pass before a unit goes to a site. The site work is in
[../pilot.md](../pilot.md).

Start from a unit that passes the M3 checklist, calibrated, and the M4 bench sessions.

## 1. What you need

| Item | Notes |
|------|-------|
| The M4 unit | Normal image, not `+inject`, except where a step says otherwise. |
| The gateway on a laptop, then on the Pi | The Pi for the modem and the cable-pull test. |
| A USB GSM modem with SMS and voice (SIM7600 or SIM800 class) and a SIM | On the gateway. Note which USB serial port answers AT commands. |
| Two phones with SIMs | One as the site contact, one as a "fire station" that stays silent. |
| The FireX release signing key | Generated once (section 3), kept offline. |

## 2. Rehearse on a laptop first

```bash
npm test                                             # gateway, escalation, site chain, firmware images, traces, export
npm run escalation:dry-run -- --incidents 20 --silent police --out reports/escalation-dry-run
python tools/pilot/pilot_report.py --demo reports/pilot-demo --out reports/pilot-demo-report
```

Read the demo report end to end. It is synthetic and says so. Then run the gateway with the
`console` provider and a simulated unit (`docs/bringup/m3-bench.md` §2), start a drill from
the dashboard, and export the site chain:

```bash
npm run gateway:cli -- drill start --services fire,police,ambulance,site
npm run gateway:cli -- site-ledger export --out site-ledger.json
npm run verify -- site-ledger.json
```

## 3. The release signing key

Generate it once, on a machine that is not the gateway, and keep it offline with a backup:

```bash
python tools/ota/sign.py keygen /secure/offline/firex-release.pem
```

It is an RSA-3072 key for Espressif's secure boot v2 signature scheme. It never goes into the
repository (the `.gitignore` refuses the usual names), onto a gateway, or into CI.

- **Losing it** means every unit has to be reflashed over USB to accept updates again.
- **M6** burns the key digest printed by `sign.py check` into eFuse for secure boot, so
  losing the key costs more then.

## 4. Upgrade the unit to two application slots (one serial flash)

The partition table changes: the factory slot becomes `ota_0`, and `otadata` and `ota_1` are
added at the end of flash. NVS (the device key, TLS key and pairing code), the ledger and the
media partition keep their offsets.

A unit accepts an update only if the image it runs is itself signed, so the first image must
be signed too. **Do not use `idf.py flash`**: it writes the unsigned image, which can never
take an update. **Never run `erase_flash`**, which destroys the device key.

1. Bump `PROJECT_VER` in `firmware/CMakeLists.txt` for every release, since a unit refuses
   an update to the version it runs.
2. Build and sign:

   ```bash
   cd firmware && idf.py set-target esp32s3 && idf.py build
   python ../tools/ota/sign.py sign --key /secure/offline/firex-release.pem \
          --in build/firex_node.bin --out build/firex_node-signed.bin
   python ../tools/ota/sign.py check build/firex_node-signed.bin      # one signature block, its key digest
   ```

3. Flash only the bootloader, the partition table, the OTA data and the signed image:

   ```bash
   esptool.py --chip esp32s3 write_flash 0x0 build/bootloader/bootloader.bin \
          0x8000 build/partition_table/partition-table.bin \
          0x610000 build/ota_data_initial.bin 0x10000 build/firex_node-signed.bin
   ```

Then on the console:

```
status          # boot count continues, ledger open
ledger key      # the same public key as before the upgrade: compare with the M2 record
ledger status   # next seq continues from before
ota status      # running ota_0, not pending verify
media list      # photos still there
```

If the ledger key changed, stop: NVS was erased.

## 5. Clock

After the unit reconnects:

```
clock           # source gateway, with the step applied
ledger tail 5   # a clock_sync entry, then entries with ts_src gateway
```

On the dashboard, the unit's clock reads "synchronised from the gateway". Restart the gateway
and check that the unit writes no new `clock_sync` for a step under 2 s.

## 6. A firmware update and a rollback

1. Bump the version, build, sign. Set the gateway's `FIREX_FW_SIGNING_KEY_SHA256` to the key
   digest (`espsecure.py digest_sbv2_public_key --keyfile <key>`, or the digest
   `sign.py check` prints), so the gateway reports images signed with that key as
   `verified`. Without it, a valid signature from any key reads `present`.
2. On the gateway:

   ```bash
   npm run gateway:cli -- firmware add build/firex_node-signed.bin
   npm run gateway:cli -- rollout <sha256> --units <device_id>
   ```

   Expect in `ledger tail`, in order:
   - `fw_update accepted`;
   - `fw_update downloaded`;
   - after a quiet minute, a restart with `reset_reason software` and a warm-up of about
     30 s;
   - within 60 s of the restart, `fw_update confirmed`.

   `ota status` shows the other slot. The rollout reads done.
3. **Rollback.** Offer another new version. When the unit restarts into it, stop the broker
   (`docker compose stop mosquitto`, or stop the dev broker), so the new image cannot
   reconnect.
   - Within 60 s, expect `fw_update failed` with "not confirmed within 60 s: not connected
     and synced to the gateway".
   - Expect a restart back into the previous slot, and a `fw_update rolled_back` naming the
     failed version, once.

   Start the broker again. The rollout reads failed and did not continue to other units.
4. **Refusals.**
   - The gateway shows an image signed with a different key as `wrong_key`, and an unsigned
     one as `missing`, and will not roll either out.
   - Offered directly over MQTT, the unit writes `fw_update failed` with "failed validation"
     and keeps running.
5. **Busy unit.** Offer an update while the unit is ELEVATED. It writes `fw_update failed`
   with "refused: the node is not in NORMAL" and stays on its image.

## 7. Traces

Hold a lighter flame 1 m from the flame sensor for 5 s, then remove it (§4 safety rules of
`docs/validation.md`).
- About 2 minutes after the unit returns to NORMAL, `trace status` shows the file closed.
- `ledger tail` shows `trace_captured`, and the dashboard lists the trace under the unit.

Label the incident `test`, then:

```bash
npm run gateway:cli -- traces export <device_id> --out traces-bench
python tools/validation/replay.py traces-bench/*.csv --out reports/replay-trace
```

The replay's fidelity line must match every state change in the trace.

## 8. The network cable

With the gateway on the Pi and the modem fitted, pull the Pi's network cable for 15
minutes. During the outage, trigger one CRITICAL with the lighter flame and let it end.
- After 10 minutes, the site phone receives "FireX: unit <label> offline since …".
- When the cable goes back, the backlog arrives in order and the chain stays complete.
- The CRITICAL arrives as a LATE alert to the site phone only.
- The site chain shows `escalation_withheld` with reason `late`, and a `heartbeat_gap` for
  the outage.

## 9. Escalation through the modem

Registry: the site phone as `site` in `live` mode, the silent phone as `fire` in `drill`
mode, and the police and ambulance recipients as the same two phones, also in `drill` mode.
1. Check the registry before loading it:

   ```bash
   npm run gateway:cli -- registry check registry.json
   ```

   It refuses public emergency numbers, a live contact without an agreement, and, for the
   modem, any sms or voice recipient that is not a phone number.
2. On the Pi, uncomment the modem's `devices` and `group_add` lines in
   `infra/docker-compose.yml`. The AT port of a SIM7600 or SIM800 on USB is usually
   `/dev/ttyUSB2`.
3. Set `FIREX_MODEM_DEVICE` to that port and `FIREX_ALERT_PROVIDER=modem`, and start the
   gateway. The start-up line must say "GSM modem on …". The registry's `ack_timeout_s` and
   `max_realerts` override the environment settings.

1. **Drill to all four contacts.**
   - Every phone gets an SMS starting "DRILL - NOT A REAL INCIDENT".
   - Voice contacts ring for 30 s.
   - Reply 1 from the site phone. The dashboard does not acknowledge drills; replies do.
   - Expect `alert_ack` from the site within seconds, and for the silent service an
     `alert_timeout` at 60 s and a re-alert.
2. **Live CRITICAL.** Trigger a CRITICAL with the lighter flame.
   - Only the site phone is alerted.
   - The site chain records `escalation_withheld` with reason `drill_mode`.
   - Acknowledge from the dashboard.
3. **Export and verify:**

   ```bash
   npm run gateway:cli -- site-ledger export --out site-ledger.json && npm run verify -- site-ledger.json
   ```

## 10. Remove a unit

Remove a second simulated or spare unit on the dashboard, then restart the broker
(`docker compose restart mosquitto`): Mosquitto reads the revocation list only at start.
- Its reconnection is refused by the broker and its firmware download by the gateway.
- `unit_removed` is in the site chain.
- It can rejoin only with a new pairing.

## 11. Pilot export and report on bench data

```bash
npm run gateway:cli -- pilot export --from <start of today> --to <now> --out pilot-bench
python tools/pilot/pilot_report.py pilot-bench --out reports/pilot-bench
```

The report lists the bench drill with every contact's times and the late alarm. Every
verifier result must be PASS. It will say "not met" for two sites, which is correct: the
bench is one site.

## 12. M5 bench exit checklist

- [ ] The unit moved to two application slots with its device key, ledger and photos intact.
- [ ] The clock syncs from the gateway, entries carry `ts_src gateway`, and no timestamp runs
      backwards.
- [ ] An update goes through accepted, downloaded, confirmed; warm-up after it is about 30 s.
- [ ] A broken update rolls back by itself, and the rollout stops.
- [ ] Unsigned and wrongly signed images are refused by the gateway and by the unit.
- [ ] An episode produces a trace whose hash is in the ledger, and its replay matches the unit.
- [ ] A 15-minute cable pull loses nothing, sends the offline SMS, and turns an ended CRITICAL
      into a LATE site-only alert with `escalation_withheld` and `heartbeat_gap` recorded.
- [ ] A drill through the modem reaches every contact, a reply of 1 acknowledges, and the
      silent contact is re-alerted no earlier than 60 s.
- [ ] A live CRITICAL with every service in drill mode alerts the site only.
- [ ] A removed unit can neither reconnect nor download firmware.
- [ ] The site chain, the unit bundles and the pilot export verify, and the pilot report lists
      the drill's who, when and how long.

## 13. Not verified yet

- **Firmware updates.** Nothing in this guide has run on a board, a Pi, a real modem or a
  real SIM. The HTTPS download from the gateway, the rollback itself, reboot timing and
  warm restart are all unexercised.
- **Bootloader.** It has not booted with `otadata` placed after the data partitions. The
  table is valid and builds.
- **Serial upgrade.** It has not been tried on a unit with a full ledger partition, or on
  one whose flash above 0x610000 is not blank.
- **Heater power.** Whether the MQ heaters stay powered through a reset from the EN button
  on the dev kit, which the 30 s warm restart assumes.
- **Modem.** AT command behaviour differs between modem models, including how inbound SMS
  and DTMF are reported. Acknowledging a voice alert by DTMF is best effort.
- **SIM rules.** Operator limits on SMS from one SIM, and the regulatory position of
  automated alerts from a personal SIM in India.
- **Large uploads.** Publishing a 58 KB trace or a photo larger than the MQTT buffer from
  the ESP32-S3.
- **Broker.** Mosquitto's handling of an updated revocation list without a restart.
