# Pilot protocol (M5)

Status: draft, M5, written before any site has been chosen. Revise it with each host before
installation, and record every revision in the table at the end.

M5 milestone (Software Roadmap v2.0): **pilot data from two live sites and a logged escalation
drill, showing who was alerted, who acknowledged, and how long each took.**

A pilot is FireX running where people live and work, alongside a site's existing fire safety
arrangements, never in place of them. FireX is not a certified fire alarm. Every unit, every
alert and every drill in a pilot has to be explainable to the host, to the people in the
building and, if something goes wrong, to an inquiry.

## 1. What each site needs

| Requirement | Why |
|-------------|-----|
| A written agreement with the host: rooms, duration, data handling, who may see photos | The media policy applies to real people from the first day. |
| A named site contact and a deputy, each with a phone that receives SMS | They receive alerts, acknowledge them and label every incident. |
| Consent notices at the door of every room with a unit, in the host's wording | A camera that captures on CRITICAL must never come as a surprise. |
| Site Wi-Fi on 2.4 GHz reaching every unit, and a fixed LAN address for the gateway | Units give the gateway's IP address; they have no mDNS. |
| Mains power for the gateway and every unit; a UPS for the gateway if the site has outages | A gateway on a flat battery cannot see, alert or record. |
| Mobile coverage for the gateway's modem, tested at the gateway's position | Alerts leave the building by SMS and voice. |
| The fire station, police station and ambulance service that cover the site, contacted in person | Registry rule 1; services stay in drill mode until they agree (§5). |
| The site's existing alarms, extinguishers and evacuation plan, unchanged | FireX is an addition under test. |

## 2. Before installation

1. **Units.** Each unit passes the M2 checklist, is calibrated (`docs/calibration.md`), runs
   the release image signed with the FireX key, never the bench `+inject` image, and has
   its pairing code on its label.
2. **Registry.** Write the site's registry (`docs/service-registry.md`): the site contact as
   `site` in `live` mode, each service in `drill` mode, recipients checked by calling them.
   - Check the file with `gateway:cli registry check`, which refuses public emergency
     numbers and live contacts without an agreement.
   - Keep every version of the file with the site's records: the site chain holds only each
     version's SHA-256.
3. **Gateway.** Build it from `infra/README.md`:
   - set `FIREX_CLOCK_SOURCE=ntp` only after checking that the host's clock is
     synchronised;
   - fit the modem and SIM, and check it with a test SMS to the site contact.
4. **Media policy.** Show `docs/media-policy.md` to the host and agree the retention window.
5. **Records.** Open the site's record sheet (§9).

## 3. Installation and commissioning

1. **Units.** Mount each unit, add it on the dashboard with its pairing code, and place it on
   the floor plan.
2. **Each unit checks.** For each unit, confirm on the dashboard that it is live, that its
   clock reads "synchronised from the gateway", and that it has passed its self-test.
3. **Cable pull.** Once per site, pull the gateway's network cable for 15 minutes.
   - During the outage, units keep alarming locally and nothing is lost.
   - After 10 minutes, the site contact receives the offline SMS.
   - When the cable is back, the backlog arrives and a `heartbeat_gap` appears in the site
     chain.

   Record the times on the sheet.
4. **Commissioning drill.** From the dashboard, start a drill to all four contacts.
   - Every contact must receive a DRILL message and acknowledge it: reply 1, or answer from
     the dashboard for the site.
   - Record each acknowledgement time. A contact that does not answer is not verified and
     is flagged until a later drill succeeds (registry rule 4).
5. **Local alarm test.** With the site contact present, run one controlled trigger per room
   (the calibration check's lighter flame at 50 cm, with the §4 safety rules of
   `docs/validation.md`).
   - Expect CRITICAL, photos, a site alert, and the services withheld while they are in
     drill mode.
   - Label the incident `test`.

## 4. Running the pilot

**The site contact.**
- Acknowledge every alert, by replying 1 or from the dashboard.
- Label every incident within 24 hours, adding a short note of what happened (toast,
  aerosol, a real fire, a test): real hazard, nuisance, or test.
- An unlabelled incident makes the pilot report incomplete.

**FireX, weekly.** Check the dashboard for:
- offline units and heartbeat gaps;
- unlabelled incidents;
- faults and failed self-tests;
- failed sends in any escalation.

Chase each one with the site contact the same week.

**Monthly.**
- Run a drill to every contact, and record every acknowledgement time.
- Take a pilot export (§7).

**Firmware.**
- An update goes to a pilot site only after it has passed the bench checks in
  `docs/bringup/m5-bench.md` on a bench unit.
- Roll it out one unit at a time from the dashboard.
- A rollback stops the rollout; find out why before offering it again.

**Never:**
- a bench image on a site;
- a test that triggers a real service in live mode without telling that service first;
- a change to the registry without the host and the service knowing. Every change is in the
  site chain.

## 5. Moving a service to live

A service moves from `drill` to `live` only with its written agreement to receive FireX
alerts. The agreement must name:
- the recipient number or endpoint;
- what the service will do with an alert;
- how the service acknowledges.

1. Record the agreement's reference and date in the registry contact's `agreement` field and
   set `mode` to `live`. The gateway writes a `registry_change` entry when it loads the file.
2. Run a drill to that service the same day and record the acknowledgement.
3. Tell the site contact and the host.

Until then, a real CRITICAL alerts the site contact only. The pilot report shows each
service's mode for the whole period.

## 6. Late alarms and outages

A unit that cannot reach its gateway still alarms locally and stores everything.
- **A CRITICAL still going when it reaches the gateway** escalates as normal, however late it
  arrived.
- **A CRITICAL that has already ended** reaches the site contact as a LATE alert, not the
  services. The site chain records the decision.
- **A site with frequent outages** produces late alarms, and those are failures of the
  installation. Fix the Wi-Fi rather than accept them.

## 7. Data collection

```bash
docker compose exec firex-server node --experimental-strip-types gateway/server/cli.ts \
       pilot export --from 2026-11-01 --to 2026-12-01 --out /data/pilot-2026-11
docker compose cp firex-server:/data/pilot-2026-11 ./pilot-2026-11
```

Each export contains:
- the site summary;
- every unit's bundle and the site chain, each with its verifier result;
- the incident and drill records with masked phone numbers;
- the episode traces in capture format.

It contains no photos. Photos stay on the gateway under the media policy, and an inquiry
fetches them there.

How an export is put together:
- **Chains** are exported whole; the period selects incidents, drills, gaps and traces by
  the time the gateway received them.
- **Trial marks** are added to a trace only for incidents labelled real hazard or nuisance.
  A trace of an ELEVATED episode, or of an incident labelled test, has none. Archive every export as it is taken. The final report is built from the
archived exports, not from a gateway that has kept running since.

## 8. The pilot report

```bash
python tools/pilot/pilot_report.py pilot-site-a-2026-11 pilot-site-b-2026-11 --out reports/pilot
```

For each site and for both together, the report shows:
- units, unit-days monitored and availability;
- incidents by label, with nuisance alarms per 100 unit-days and their interval;
- every drill and every live alert: who was alerted, when it was sent, delivered and
  acknowledged, by whom and after how long, with timeouts and failures;
- firmware history with confirmations and rollbacks;
- self-tests and faults;
- the traces replayed through the M4 harness;
- every verifier result.

It ends with the M5 criteria marked met, not met, or no data.

What it cannot show:
- how often a real hazard raised no score at all, since traces exist only around episodes.
  Missed detection is the M4 bench measurement.
- anything the site contact did not label.

## 9. Record sheet, per site

| Field | Value |
|-------|-------|
| Host, agreement reference and date | |
| Site contact and deputy | |
| Rooms and unit labels, unit IDs, firmware version at install | |
| Registry SHA-256 at install, service modes and agreements | |
| Gateway IP, clock source, modem model, SIM number (masked) | |
| Cable-pull test: start, offline SMS received, end, heartbeat gap recorded | |
| Commissioning drill: per contact, sent, delivered, acknowledged, by whom | |
| Local alarm test per room: CRITICAL time, photos, site alert, services withheld | |
| Monthly drills: date and per-contact acknowledgement times | |
| Firmware rollouts: version, date, result | |
| Exports: date, period, directory, verifier results | |
| Incidents raised with the host, and how they were resolved | |

## 10. Ending a site

Take the final export. Remove each unit on the dashboard, which revokes its certificate.
Collect the units. Agree with the host what happens to the photos still inside the retention
window. Archive the gateway's data volume, including the site chain, the site CA and the
site-ledger key, because they are what the archived exports are verified against.

## Revisions

| Date | Change | Why |
|------|--------|-----|
| 2026-09-15 | First version | M5 |
