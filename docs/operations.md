# Operations

Status: draft, M6. How FireX keys, firmware, gateways and units are looked after, from the
factory to decommissioning. Nothing here has been run with a production unit. The gateway's own
day-to-day procedures are in [infra/README.md](../infra/README.md), the pilot protocol in
[pilot.md](pilot.md), and what each safeguard protects against in
[threat-model.md](threat-model.md).

## 1. Who holds what

| Secret or record | Held by | Where | Backup | If lost |
|------------------|---------|-------|--------|---------|
| Release signing key (RSA-3072) | The release manager, one named person | Offline, never on a gateway, in CI or in the repository | Two offline copies in different places | No update for any unit, ever. A production unit cannot boot any other image, so there is no recovery short of replacing the board |
| Site CA key | The site gateway | `firex_firex-data` volume | Encrypted gateway backup | Every unit at the site must be added again with a new pairing |
| Site-ledger key | The site gateway | Same volume | Same backup | The site chain ends; a new one starts under a new key. The old one still verifies |
| Gateway database and photos | The site gateway | Same volume | Same backup | Mirrored records and photos not yet exported are lost. Units still hold their recent ledger |
| Device keys | Each unit | Dev kit: NVS. EVT: ATECC608B | None, by design | The unit gets a new identity: new device ID, new pairing, new chain |
| Factory manifest | FireX | The factory station, then FireX's records | Copied off the station after every session | The record of what each unit left the factory with |
| Pairing code label file | The factory station, briefly | `labels.csv` | **None**: destroyed after printing | Nothing: the code is on the unit and its label |

The release key's public digest is not secret. Record it with each release and give it to the
factory station and to every gateway (`FIREX_FW_SIGNING_KEY_SHA256`).

## 2. The site gateway

- **Backups.** Follow infra/README.md §6: stop the server, archive the volume, start it again.
  The archive holds the CA key, the site-ledger key and photos, so keep it encrypted and off the
  Pi.
- **Restore test.** Restore a backup to a spare Pi and open the dashboard before a site goes
  live, and again after every gateway upgrade. A backup that has never been restored is a guess.
- **Upgrades.** infra/README.md §7. Migrations run at start.
- **Retention.** Photos are deleted at the end of the window (30 days by default,
  media-policy.md). Export a period with `pilot export` or `export` first if an inquiry may
  need it.
- **A gateway that is stolen or compromised.** Treat the site CA and site-ledger key as known to
  someone else:
  1. Build a new gateway with a new CA and a new site-ledger key.
  2. Add every unit again with a new pairing.
  3. Keep the old site chain and its public key for verification.
  4. Record the event in the new site chain's first days with a `registry_change` note, and in
     writing to the site.

## 3. The release signing key

- **Generate** it once, on a machine that is not a gateway and has no network while the key is
  on it: `python tools/ota/sign.py keygen <offline path>/firex-release.pem`.
- **Copy** it to two offline media kept in different places. Write down where they are and who
  can reach them.
- **Use** it only on an offline machine, to sign images copied to it and back.
- **Never** put it in the repository (the `.gitignore` refuses the usual names), on a gateway,
  or in CI. CI builds unsigned images.
- **Suspected compromise.**
  1. Stop all rollouts at once.
  2. On dev-kit and EVT development units, a new key can be introduced only by a serial flash.
  3. On production units the key digest is burned into eFuse and the unused digest slots are
     revoked, so the key cannot be replaced. The units keep accepting images signed with the
     compromised key.
  4. Tell the sites, and plan board replacement (threat-model.md §8).

## 4. Firmware release process

1. **Change and review** in a pull request. CI builds all three node images and both camera
   images, and runs every host test.
2. **Bump `PROJECT_VER`** in `firmware/CMakeLists.txt`. A unit refuses the version it runs.
3. **Build** each image in its own build directory, on the release machine:

   ```bash
   cd firmware
   idf.py -B build-devkit -D SDKCONFIG=build-devkit/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" set-target esp32s3 build
   idf.py -B build-evt -D SDKCONFIG=build-evt/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.evt" set-target esp32s3 build
   idf.py -B build-evt-production -D SDKCONFIG=build-evt-production/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.evt;sdkconfig.production" set-target esp32s3 build
   ```

4. **Sign offline.** Every application image is signed. The production bootloader is signed too,
   for the factory; units in the field never receive a bootloader.

   ```bash
   python ../tools/ota/sign.py sign --key <key> build-evt-production/firex_node.bin
   python ../tools/ota/sign.py sign --key <key> build-evt-production/bootloader/bootloader.bin
   python ../tools/ota/sign.py check build-evt-production/firex_node-signed.bin   # one block, the release key digest
   ```

5. **Name the image by its kind.** Dev kit, EVT development and EVT production images share the
   project name `firex_node` and the release key. Each carries its kind (`devkit`, `evt`,
   `evt-production`) inside the signed image, `sign.py check` prints it, and a unit refuses an
   update of another kind (ADR 0009). Keep the three apart in file names and release notes
   anyway, so a rollout is not refused unit by unit.
6. **Add and roll out** on the gateway (infra/README.md §8): `firmware add`, then `rollout` to
   one unit, and to the rest once it has confirmed.
7. **Record** the version, the SHA-256 of each signed image, the key digest and the sites it went
   to. Every offer is already an `fw_offered` entry in each site chain.

The camera's production image is not updated in the field: it has no network, and its ROM loader
is disabled (ADR 0009).

## 5. A unit's life

1. **Factory provisioning.** `tools/factory/provision.py` on a blank EVT board
   ([m6-bench.md](bringup/m6-bench.md) §6):
   - writes the signed production images;
   - lets the first boot burn the security eFuses;
   - provisions and locks the secure element;
   - checks `security status` against the expected state;
   - appends the unit to the manifest.

   Every step is irreversible.
2. **Label.** The unit serial, the device ID and the pairing code. Check the printed code
   against the unit's `pairing` output before the label file is destroyed.
3. **Installation and pairing.** infra/README.md §5 and m3-bench.md §5. Compare the device ID
   the gateway registers with the manifest.
4. **In service.** Daily self-tests, updates by rollout, heartbeat gaps and faults on the
   dashboard.
5. **Removal.** infra/README.md §9: the certificate is revoked, `unit_removed` goes into the site
   chain, and the broker must be restarted. The unit's records and photos stay on the gateway.
6. **Repair or return.**
   - A production unit cannot be reflashed over serial: flash encryption is in release mode and
     the ROM loader is restricted.
   - A locked ATECC608B cannot be given a new key.
   - A unit with a faulty board is replaced, not re-keyed. The replacement has a new device ID
     and needs a new pairing. The old unit's chain still verifies under its old key.
   - A unit whose firmware can still run is updated by rollout like any other.
7. **Decommissioning.**
   1. Export the unit's bundle and verify it.
   2. Remove the unit on the gateway.
   3. Keep the export with the site's records.
   4. The device key cannot be erased. Destroy the secure element, or keep the unit in FireX's
      custody, so it can never sign again.

## 6. Factory records

The factory tool writes two files per session, in a directory outside the repository:

- **`manifest.jsonl`**: one JSON record per unit. Each record carries the SHA-256 of the record
  before it, so a removed or edited line is detected by
  `python tools/factory/provision.py verify-manifest <file>`. It holds the unit serial, the
  device ID and ledger public key, the firmware version and hash, the release key digest, the
  eFuse state read back, and the outcome. It does not hold the pairing code, only its hash.
  Copy it off the station after every session.
- **`labels.csv`**: the pairing code for each unit, for the label printer. Print, check each
  label against the unit, then delete the file. The repository's `.gitignore` refuses CSV files,
  but the file should never reach a shared drive either.

## 7. Vulnerability disclosure and patches

- **Contact.** `<security contact to be published before the first production unit ships>`.
  The contact and a public key for encrypted reports go in the README and on the product's web
  page.
- **On a report.**
  1. Acknowledge it.
  2. Reproduce it on a dev kit or an EVT development unit.
  3. Assess which units and sites are exposed.
  4. Agree a disclosure date with the reporter.

  Response and fix targets are set before the first production unit ships.
- **Patch.**
  1. Fix in a reviewed pull request, released through §4.
  2. Roll out to one unit per site, then the rest.
  3. Write an advisory naming the affected versions, the fixed version, and what a site should
     check.
- **If a fix cannot reach a unit** (the camera image, the bootloader, or anything burned into
  eFuse), say so in the advisory, with the mitigation.
- **Support period.** How long a firmware line receives security fixes is stated before the first
  production unit ships.

## 8. Not verified yet

- Nothing in §5 or §6 has been done with a production unit. The PCB does not exist.
- A gateway restore on a spare Pi.
- The factory tool against a real board, its manifest format against a year of use, and the
  label workflow.
