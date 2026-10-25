# ADR 0005 — The site gateway

Date: 2026-09-15
Status: accepted for M3-M5

## Context

M3 connects each node to a gateway on the site LAN: MQTT over mutual TLS, a mirror of every
unit's ledger, incident photos, dashboard v1 and a way to add a unit. The gateway is a
Raspberry Pi that may have no internet, at a site with no IT staff, built and run by one
developer. ADR 0001 asked for running TypeScript without a build step to be revisited here.

## Decisions

**One process, one SQLite file.** `firex-server` is a single Node 22 process: the MQTT
bridge, ingest, the HTTP API and dashboard, the provisioning endpoint and the retention job.
State is one SQLite database through `node:sqlite`, which ships with Node, so nothing native
is compiled on a Pi. WAL mode, foreign keys, and migrations numbered in the database itself.
It is loaded through `createRequire`, because Vite's resolver in the test runner otherwise
looks for an npm package named `sqlite`.

**Type stripping stays, in the container too.** This closes ADR 0001's revisit. The image
runs the same `.ts` sources with `--experimental-strip-types` that the tests and the laptop
run, so there is one code path to reason about. The dashboard is the only build step, and
it needs one anyway for JSX and bundled fonts. The cost is an experimental flag in
production; revisit when Node drops the flag or if start-up time on the Pi starts to matter.

**A certificate authority per site, on the gateway.** On first start the gateway creates a
P-256 CA and issues the broker's server certificate and its own MQTT client certificate. A
unit receives a client certificate at provisioning, with the device ID as its common name
and client authentication as its only use. No public CA and no internet are involved.
Mosquitto takes the certificate's common name as the username and applies an access list
keyed on it: a unit publishes only under its own device ID and reads only its own commands.

**Provisioning by pairing code, authenticated both ways, not encrypted.** The code printed
on the unit is the only secret the installer handles. The unit proves it holds the code
with an HMAC and proves it holds its ledger and TLS keys with two signatures; the gateway
proves it holds the code with a MAC over its reply, so a device on the LAN cannot hand the
unit a rogue CA. The exchange is plain HTTP because the unit has no trust anchor before it
is provisioned. A recorded exchange allows an offline search of the 50-bit code, which gains
nothing once the pairing is used. Pairings expire after 24 hours, are single-use, and failed
attempts are rate-limited. The contract is `docs/transport.md`.

**The ledger is the outbox.** The node keeps no second queue on flash. After every connect
it announces its oldest and next entry; the gateway answers with the last entry it holds,
and the node sends from there, at most eight entries past the last acknowledgement. The
gateway acknowledges duplicates again, holds entries that arrive out of order, and asks for
a resend when it sees a hole. Holes that rotation made permanent are recorded as chain
events and a gap status, and a restarted chain becomes a new epoch. Nothing is papered over.

**Every entry is checked on arrival; the export is what counts.** The gateway verifies link,
hash and signature as each entry lands and shows the verdict. An inspector does not have to
trust that: the log bundle the gateway exports is checked by the reference verifier, the
same one that checks a bench export over USB.

**Photos on their own topic, addressed by hash.** A photo is published to `media/<sha256>`.
The gateway refuses bytes that do not hash to the topic, stores the file under the unit, and
acknowledges it; only then does the node delete its copy. Retention deletes files and keeps
the database row, so the dashboard can say a photo existed and was deleted on schedule.

**Aedes in the tests, Mosquitto on the Pi.** The end-to-end test runs an in-process Aedes
broker over TLS with the same access rules written in TypeScript, so CI needs no Docker to
exercise provisioning, mirroring, outages, rotation gaps, a rogue CA and a forged publish.
The Compose stack runs Mosquitto 2 with the equivalent ACL file.

**React and Vite for the dashboard, fonts bundled.** The site LAN may have no internet, so
nothing loads from a CDN. The gateway serves the built dashboard; Caddy in front terminates
HTTPS with its own local CA.

**Accounts local to the gateway.** scrypt password hashes, viewer, operator and admin roles,
12-hour sessions in an HttpOnly cookie, and an audit log for exports, photo views, pairings,
labels and account changes.

## Consequences

- The M3 bench demonstration needs no cloud account and no internet connection.
- The gateway's data volume holds the database, the photos and the CA. Back them up
  together. Losing the CA means provisioning every unit again, each with a new pairing,
  because pairings are single-use.
- A gateway outage takes away the site's view, not its alarms. Units keep alarming and
  storing entries, and catch up when the gateway returns, within their storage window.
- The access rules exist twice, in `gateway/sim/acl.ts` and `infra/mosquitto/acl`, and are
  kept in step by hand. A change to one without the other passes CI.
- Units do not resolve `.local` names: the firmware has no mDNS. The gateway must advertise
  an IP address or a DNS name the site's router resolves.
- If the provisioning reply is lost after the gateway marked the pairing used, the unit
  gets 403 on its retry. The operator adds the pairing again with the same code.
- Nothing here has run on a Raspberry Pi or a board yet. The laptop rehearsal in
  `docs/bringup/m3-bench.md` runs the gateway, the dev broker and simulated units.
