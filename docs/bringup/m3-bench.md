# M3 bench: node to gateway to dashboard

Status: the gateway, the dashboard and provisioning have run on a laptop against simulated
units (section 2). The node's network code compiles against ESP-IDF 5.5.5 but has not run
on a board, and the Compose stack has not run on a Raspberry Pi.

M3 milestone (Software Roadmap v2.0): **a unit on the bench mirrors its ledger to the site
gateway over mutual TLS, its incident appears on the dashboard, and the exported log
verifies with the incident photo's hash checking out.**

Start from a node that passes the M2 checklist in [m2-bench.md](m2-bench.md). The protocol is
in [docs/transport.md](../transport.md) and the design in
[ADR 0005](../adr/0005-site-gateway.md).

## 1. What you need

| Item | Notes |
|------|-------|
| The M2 node with its camera | Upgrading keeps its device key, ledger and photos: the partition table is unchanged. |
| A Raspberry Pi 4 or 5 with 64-bit Raspberry Pi OS and Docker | Or a laptop for the rehearsal in section 2. |
| A 2.4 GHz Wi-Fi network that the node and the gateway both join | Reserve a fixed address for the gateway on the router. |
| A USB cable, or a phone with Espressif's SoftAP provisioning app | Either one gives the node its network. The USB console also sets the gateway address. |

## 2. Rehearse on a laptop first

This runs the real gateway, a TLS broker with the site's access rules, and simulated units
that provision, mirror entries and upload photos with the same protocol as the firmware. It
is how the dashboard was reviewed before any board existed.

```bash
npm install && npm run dashboard:build
export FIREX_DATA_DIR=$PWD/gateway-data FIREX_SITE_NAME="Bench site" \
       FIREX_MQTT_ADVERTISED_HOST=localhost FIREX_BROKER_NAMES=localhost,127.0.0.1
npm run gateway -- --init-only        # creates the site CA and the broker certificate
npm run dev:broker                    # terminal 1: MQTT over mutual TLS on 8883
npm run gateway                       # terminal 2: dashboard on 8080, provisioning on 8081
npm run gateway:cli -- user add admin --role admin    # asks for a password of 12+ characters
```

Add a unit and run its incident:

```bash
npm run gateway:cli -- pairing new --label "Room 206" --x 0.3 --y 0.6
# prints the pairing code first, for example 7K2QD-M9XWE
npm run sim:node -- --code 7K2QD-M9XWE --provision http://127.0.0.1:8081 \
       --state room206.json --scenario incident
```

Open http://localhost:8080 and sign in. The unit is on the site plan, its incident and three
photos are under Incidents, and every timeline entry reads Verified. Run the same state file
again with `--scenario normal --stay` to keep the unit live; other scenarios are `elevated`,
`critical` and `idle`.

Export the log and check it the way an inspector would:

```bash
npm run gateway:cli -- devices                       # the device ID
npm run gateway:cli -- export <device_id> --out export-room206
npm run verify -- export-room206/bundle.json --media export-room206/media
```

The verifier answers PASS, with the photos checked against the hashes in the log.

## 3. The gateway on the Pi

Follow [infra/README.md](../../infra/README.md): set `site.env`, start the stack, create the
first admin, and install the dashboard's certificate on the desk PC. Check that
`https://<FIREX_HOSTNAME>` shows the sign-in page before going on.

## 4. Flash the M3 node image

```bash
cd firmware && idf.py set-target esp32s3 && idf.py build flash monitor
```

Settings are in `idf.py menuconfig` → FireX node → Network (M3). Expect, on the first boot of
the M3 image:

```
pairing code 7K2QD-M9XWE
W (…) net: a new TLS key was generated on this boot
onboarding: open the ESP SoftAP Provisioning app, join FIREX-3A91C4, proof of possession 7K2QD-M9XWE
```

**Write the pairing code on a label on the unit now.** It never changes, it is the only way
to add the unit to a site, and anyone who reads it can add the unit to their own gateway
while a pairing for it is open. `pairing` on the console prints it again.

## 5. Put the unit on the site network and add it to the gateway

Give the node its network one of two ways.

On the USB console:

```
firex> wifi set "Site Wi-Fi" "the password"
firex> gateway set 192.168.1.10
```

Or from a phone: Espressif's provisioning app, SoftAP transport, join `FIREX-…`, enter the
pairing code as the proof of possession, then choose the network. The gateway address cannot
be entered in every version of the app. `esp_prov.py --transport softap --sec_ver 1 --pop
<code> --custom_data 192.168.1.10` from ESP-IDF sends it; otherwise use `gateway set`.

Then, on the dashboard, choose **Add a unit**, type the code from the label, give the unit a
name and place it on the floor plan. The node tries to provision every 30 seconds;
`net provision` makes it try now. Expect:

```
I (…) provision: …
I (…) net: provisioned into site site-001; broker 192.168.1.10 port 8883
I (…) net: connecting to mqtts://192.168.1.10:8883 as 3a91c4…
I (…) net: connected to the gateway broker
```

`net status` shows the network, the gateway, and how far the gateway's copy of the ledger
has got. A 403 in the log means the pairing is not on the dashboard yet, or has expired.

Use the gateway's IP address, not a `.local` name: the firmware has no mDNS.

## 6. The milestone demonstration

1. **Normal.** The unit's hex on the site plan is green and the header says Live. Its
   timeline shows the boot, the self-test and the move out of warm-up, each Verified.
2. **Incident.** Drive the unit to CRITICAL as in section 5 of the M2 guide. The band across
   the dashboard turns red with the unit's name, Incidents shows the photos, and each
   caption reads "Matches" with the hash recorded in the `media_captured` entry. On the
   console, `media list` shows the photos gone once the gateway has acknowledged them.
3. **Outage.** Switch off the Wi-Fi access point for ten minutes, cause another state
   change, and switch it back on. The unit catches up by itself: the timeline fills in and
   the unit's record on the Ledger page still reads complete and verified.
4. **Export.** On the Pi:

   ```bash
   docker compose exec firex-server node --experimental-strip-types gateway/server/cli.ts \
          export <device_id> --out /data/export
   docker compose cp firex-server:/data/export ./export
   ```

   On any other machine with the repository:

   ```bash
   npm run verify -- export/bundle.json --media export/media
   ```

   The answer is PASS. Change one byte in one of the photos and run it again: the photo no
   longer matches the hash in the record.
5. **Label.** Mark the incident as a real hazard, a nuisance or a test on the Incidents page.
   The label is stored with the account that set it.

## 7. M3 exit checklist

- [ ] The unit is added with its pairing code; the same code, used again, is refused.
- [ ] A client certificate from any other CA is refused by the broker on 8883.
- [ ] The dashboard shows each state change within a few seconds of the console.
- [ ] CRITICAL opens an incident whose photos the dashboard shows as matching their hashes.
- [ ] A ten-minute Wi-Fi outage loses no entries: the record stays complete.
- [ ] A gateway restart (`docker compose restart`) loses no entries and adds no duplicates.
- [ ] Photos are deleted from the node only after the gateway acknowledges them.
- [ ] The exported bundle and photos verify PASS on a machine that is not the gateway.
- [ ] Opening a photo and exporting a log each appear in the audit log with the account.

## 8. Not verified yet

- Nothing in the node's network code has run on a board: Wi-Fi onboarding, the provisioning
  client, mutual TLS from the ESP32-S3, and memory use while a photo is published.
- Units verify the broker's certificate against an IP address. mbedTLS supports IP
  addresses in certificates, but this has not been tried against the gateway's certificate.
- Whether the phone app can send the gateway address, per app version.
- The Compose stack on a Pi: image build time on ARM, SD card wear from SQLite, and Caddy's
  local certificate on the desk PCs.
- The broker access rules are tested in CI on Aedes with the rules in `gateway/sim/acl.ts`,
  not on Mosquitto with `infra/mosquitto/acl`.
- Time is still `uncertain` in every entry: the gateway does not yet set the node's clock.
