# ADR 0007 — What the pilot sites run

Date: 2026-09-15
Status: accepted for M5

## Context

M5 puts units in two live sites. Alerts go to real people, firmware has to change without
a laptop on a USB cable, and the data has to support a report that says who was alerted,
who answered and how long it took. M4 left three questions open (ADR 0006):
- who signs alert events, since the gateway holds no unit's key;
- what happens to an escalation when the gateway restarts;
- what a CRITICAL that arrives late, after an outage, should do.

The node still has no battery-backed clock. The sites may have no internet, and no
emergency service has signed an agreement.

## Decisions

**A site chain, signed by the gateway.** Alert, acknowledgement and timeout events, heartbeat
gaps, incident labels, drills, registry changes, firmware offers and unit removals go into
a second chain. It uses the ledger's own entry format and is signed by a P-256 key the
gateway generates at first start. Entries about an incident name the unit's CRITICAL entry
by seq and hash. The reference verifier checks the site chain unchanged.

Rejected:
- Asking the unit to sign the gateway's events. A unit in a fire may be gone before its
  alert is acknowledged, and a unit would then sign claims it cannot check.
- Keeping them only in the database, which an inquiry has no reason to trust.

**The node's clock comes from its gateway.** The gateway sends its time over the mutually
authenticated MQTT link after every sync and hourly. Entries then carry `ts_src` =
`gateway`, the first sync and any step of 2 s or more write `clock_sync`, and `ts` never
runs backwards within a chain.

Rejected: SNTP, which is unauthenticated on the LAN and adds a second service to the
gateway, for precision the one-second `ts` cannot use.

**Signed updates, two application slots, confirm or roll back.**
- **Layout.** The partition table gains a second application slot and OTA data. NVS (the
  device key), the ledger and the media partition stay at their offsets, so an existing
  unit keeps its identity and record through the one serial upgrade.
- **Signing.** Images are signed offline with Espressif's secure boot v2 signature scheme,
  and a unit checks the signature before it switches slots. The key is kept offline, and M6
  turns on secure boot with the same key.
- **Confirmation.** A new image must confirm within 60 s: ledger open, alarm and fusion
  running, the same sensors ready as before, and reconnected to the gateway. Otherwise the
  bootloader returns to the previous image.

  The gateway connection is part of the check on purpose. An image that cannot talk to the
  site can never be updated again, and rolling it back is the only remote repair.
- **Timing and warm-up.** A unit updates only in NORMAL and restarts only after 60 quiet
  seconds. Warm-up after a restart that kept the heaters powered is 30 s rather than three
  minutes.

**Traces around episodes, not continuous recording.** When fusion leaves NORMAL, a unit
records the raw scores from 5 minutes before until 2 minutes after it returns, capped at 30
minutes. It hashes the file into the chain with `trace_captured` and sends it to the gateway
like a photo. Continuous raw capture from two sites would cost flash, Wi-Fi and a retention
problem, for data nobody can label. The site contact labels episodes, and labelled episodes
replay through the M4 harness.

**A GSM modem on the gateway for SMS and voice, HTTP kept.**
- **The modem.** A USB modem and a SIM on the Raspberry Pi need no internet, no aggregator
  account and no build step: AT commands over a serial device, with no native module.
- **Messages.** A site contact acknowledges by replying 1. A voice alert rings the
  recipient's phone and hangs up: it carries no spoken message, and is a prompt to read
  the SMS.
- **HTTP.** The HTTP provider stays for an aggregator later, and for the mock endpoints.

**Services stay in drill mode until an agreement exists.** Each contact in the registry is in
`drill` or `live` mode, and `live` requires an agreement reference.
- **Real alarms.** A real CRITICAL alerts the site contact always, and a service only in
  `live` mode.
- **Drills.** Drills reach every chosen contact, and every message says DRILL.
- **Emergency numbers.** The registry refuses the public emergency numbers outright.
- **Changes.** Every change to the registry is a `registry_change` in the site chain.

**Late alarms go to the site, not the services.** When an incident opens, the gateway waits
for that unit's backlog to drain. If the CRITICAL has already ended in the backlog and is
more than two minutes old, or its time is uncertain, the site is told it was reported late
and the services are not. The site chain records `escalation_withheld` with reason `late`.
A CRITICAL that is still going when it arrives is escalated as normal, however late it
arrived.

**Escalations resume after a restart.** The gateway rebuilds unfinished escalations younger
than an hour from its event tables, so a restart mid-incident does not silently end the
chasing.

**Revocation through the site CA.** Removing a unit puts its certificate serial on a
revocation list that the broker and the firmware server check, and writes `unit_removed`.

**One export per site, one report across sites.** The gateway exports a site's period as a
directory:
- the incidents with their labels and alert timelines, and the drills;
- unit uptime and gaps, firmware history, and the traces in capture format;
- every unit bundle and the site chain, each with its verifier result.

The pilot report reads any number of these exports.

## Consequences

- An inquiry verifies two kinds of chain for one site: each unit's, and the site's. The
  cross-references let it check that an alert names a CRITICAL the unit actually recorded.
- The site-ledger key and the site CA sit in the gateway's data volume in plain form. Back
  them up together. A hardware key store for the gateway is not planned before Q3.
- Units depend on the gateway for wall-clock time. A unit that never reaches its gateway
  after a reboot writes `uncertain` times, and its pilot data says so.
- Signing needs a key held by one person offline. Losing it means no further remote
  updates, and in M6 no secure boot with that key.
- Every existing unit needs one serial flash to move to two application slots. After that,
  updates are remote.
- The modem is a single point of failure for SMS and voice. A failed send is recorded and
  shown on the dashboard, and the site still has the unit's own buzzer. Operator limits on
  messages from a personal SIM, and India's rules on commercial messaging, must be checked
  before the pilot rather than assumed.
- A voice alert carries no information of its own. A service that wants a spoken message
  needs an aggregator or an agreement-specific integration, which is Q3.
- Late alarms reach services only if they are still happening. This is a judgement: an
  inquiry may ask why a fire station was not told about a fire the unit had already
  alarmed on locally. The site chain records every such decision.
- Traces show only episodes, so pilot data cannot measure how often a real hazard failed to
  raise any score. Missed detection stays an M4 bench measurement.
