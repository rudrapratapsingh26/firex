# The site gateway on a Raspberry Pi

Status: written before it has run on a Pi. The same server, broker rules and dashboard have
run on a laptop (docs/bringup/m3-bench.md, section 2). Correct this page when the Pi
disagrees.

One `docker compose up` starts three containers:

| Container | What it does | Port on the LAN |
|-----------|--------------|-----------------|
| `firex-server` | Ingest from units, provisioning, the site CA, the API and dashboard, retention | 8081 (provisioning) |
| `mosquitto` | MQTT broker, mutual TLS, per-unit access rules | 8883 |
| `caddy` | HTTPS for the dashboard with its own local CA | 443, and 80 to redirect |

`firex-server` also listens on 8443, where units download firmware during a rollout, with
their client certificates (section 8).

Nothing needs the internet once the images are built.

## 1. Prepare the Pi

- A Raspberry Pi 4 or 5 with 4 GB or more, 64-bit Raspberry Pi OS Lite, and a good SD card or
  an SSD.
- A fixed LAN address, reserved for the Pi on the site's router. Units are given this
  address, not a name.
- A clock that is right. Received times, sessions and retention use the Pi's clock. Keep
  `systemd-timesyncd` on and give it an NTP server it can reach, on the LAN if there is no
  internet.
- Docker with the Compose plugin:

  ```bash
  curl -fsSL https://get.docker.com | sh
  sudo usermod -aG docker "$USER"      # then log out and in again
  git clone <the FireX repository> firex
  ```

## 2. Configure and start

```bash
cd firex/infra
cp site.env.example site.env
nano site.env
docker compose --env-file site.env up -d --build
docker compose ps                     # firex-server healthy, mosquitto and caddy up
```

| Setting | Meaning |
|---------|---------|
| `FIREX_SITE_ID` | A short ID for the site, used in MQTT topics. It cannot change once units are added. |
| `FIREX_SITE_NAME` | The name at the top of the dashboard. |
| `FIREX_HOSTNAME` | The name desk PCs use for the dashboard. Add it to the router's DNS, or to each desk PC's hosts file. |
| `FIREX_GATEWAY_IP` | The Pi's fixed address. Units use it for MQTT and provisioning, and it goes into the broker's certificate. Required. |
| `FIREX_MEDIA_RETENTION_DAYS` | Days before incident photos are deleted, 30 by default. See docs/media-policy.md before lengthening it. |

On first start `firex-server` creates the site CA and the broker's certificate in its data
volume. Mosquitto waits for that before it starts. Changing `FIREX_HOSTNAME` or
`FIREX_GATEWAY_IP` later reissues the broker's certificate at the next start; units keep
working because they trust the CA, not the certificate, but they connect to the address
they were given when they were added.

## 3. The first administrator

```bash
docker compose exec firex-server node --experimental-strip-types gateway/server/cli.ts user add admin --role admin
```

It asks for a password of at least 12 characters. Add operators, who can add units and
label incidents, and viewers, who can only look, the same way with `--role operator` or
`--role viewer`.

## 4. Trust the dashboard's certificate on each desk PC

Caddy signs the dashboard's certificate with its own root. Copy the root off the Pi once:

```bash
docker compose cp caddy:/data/caddy/pki/authorities/local/root.crt ./firex-dashboard-root.crt
```

- **Windows:** open the file, choose Install Certificate, Local Machine, and place it in
  Trusted Root Certification Authorities.
- **macOS:** open it in Keychain Access, then set it to Always Trust.
- **Firefox** keeps its own list: Settings, Privacy and Security, View Certificates,
  Authorities, Import.

Then open `https://<FIREX_HOSTNAME>` and sign in.

## 5. Adding units

On the dashboard, **Add a unit** takes the pairing code from the unit's label, a name and a
place on the floor plan. The unit needs the site network and the gateway's address; see
docs/bringup/m3-bench.md, section 5. A pairing lasts 24 hours and works once.

Port 8081 is only used while units are being added. It can be closed on the Pi's firewall
the rest of the time.

## 6. Backups

The `firex_firex-data` volume holds everything that matters: the database, the photos and
the site CA's private key. Back it up with the server stopped, so the database is consistent:

```bash
docker compose stop firex-server
docker run --rm -v firex_firex-data:/data -v "$PWD":/backup alpine \
       tar czf /backup/firex-data-$(date +%F).tgz -C /data .
docker compose start firex-server
```

Keep backups off the Pi and encrypted: they contain the CA key and incident photos. Losing
the CA without a backup means adding every unit again, each with a new pairing.

## 7. Upgrades, logs and health

```bash
git pull && docker compose --env-file site.env up -d --build   # migrations run at start
docker compose logs -f firex-server
curl -k https://<FIREX_HOSTNAME>/api/healthz
```

## 8. Firmware updates

A node image is added to the gateway, checked, and offered to one unit at a time.

```bash
docker compose cp firex_node-0.5.0-signed.bin firex-server:/tmp/node.bin
docker compose exec firex-server node --experimental-strip-types gateway/server/cli.ts firmware add /tmp/node.bin
docker compose exec firex-server node --experimental-strip-types gateway/server/cli.ts rollout <sha256> --units all
```

The dashboard's firmware page does the same. On upload the gateway checks the image before
it will offer it:
- it is an application image for `firex_node`, with a version;
- its secure boot v2 signature verifies (RSA-PSS);
- the signing key's digest matches `FIREX_FW_SIGNING_KEY_SHA256`, which you set once per site
  with the output of `espsecure.py digest_sbv2_public_key`.

An unsigned image, or one signed by another key, is kept but never rolled out: the unit
would refuse it.

A rollout sends the first unit an `ota` command. The unit downloads the image from port
8443 and installs it when it is quiet, then confirms the new image within 60 seconds of
booting it. Only then is the next unit offered the image.
- A unit that fails to confirm rolls back on its own, and the rollout stops. The units still
  waiting keep their old image.
- A unit that makes no progress for 15 minutes stops the rollout too.
- Offline units, removed units, and units already on the version are skipped, with the
  reason.

Every offer is a `fw_offered` entry in the site ledger; every step the unit takes is a
`fw_update` entry in its own.

Port 8443 is needed only while a rollout runs.

## 9. Removing a unit

```bash
docker compose exec firex-server node --experimental-strip-types gateway/server/cli.ts device remove <device_id>
docker compose restart mosquitto
```

Removing a unit marks it removed, revokes its client certificate in the site CRL
(`<data>/pki/crl.pem`) and writes a `unit_removed` entry to the site ledger. The gateway
ignores the unit's messages and refuses its firmware downloads at once. **Mosquitto reads the
CRL only when it starts**, so restart it; until then the unit can still connect to the broker,
though nothing it sends is taken. The unit's records, photos and traces stay on the gateway.
To bring the unit back, add it again with a new pairing, which issues it a new certificate.

## 10. The clock

Set `FIREX_CLOCK_SOURCE=ntp` in `site.env` once `timedatectl` on the Pi reports the clock as
synchronised. The site ledger records each entry's time source, and units take their time
from the gateway after every connection and every hour, so a Pi with a wrong clock gives
every unit a wrong clock too.

## 11. Not verified yet

- The stack has not run on a Pi, and the images have not been built for ARM.
- Mosquitto reads its key from the shared volume as uid 1000, the `node` user in the
  `firex-server` image. Confirm the broker starts after the first `up`.
- Caddy's certificate on the desk PCs, per browser.
- Mosquitto with `crlfile`: that it starts with the gateway's empty CRL, and that a restart
  refuses a removed unit. The CRL itself is exercised only by Node's TLS in the tests.
- A rollout to a real unit: the download over 8443, the unit's install and its confirmation.
