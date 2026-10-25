# Site service registry

Status: version 2, M5. Defines how a site is mapped to the emergency services that cover it,
which of them receive real incidents, and how the gateway reaches them. Version 1 files (M0 to
M4) still load, with every service a drill contact.

The mapping is made **at installation and stored on the site gateway**. Nothing is looked up
during an alarm: a node that has just confirmed a fire is not the moment to be resolving a
jurisdiction.

## Shape

```jsonc
{
  "version": 2,
  "site_id": "site-001",
  "site_name": "Hostel, Block B",
  "address": "Block B, example campus",
  "location": { "lat": 0.0, "lon": 0.0 },
  "ack_timeout_s": 60,
  "max_realerts": 1,
  "offline_notify_min": 10,
  "contacts": [
    { "service": "site", "name": "Warden on duty", "recipient": "+919800000001", "channel": "sms", "mode": "live" },
    { "service": "fire", "name": "Fire station 42", "recipient": "+919800000042", "channel": "sms",
      "mode": "live", "agreement": { "reference": "FS-42/2026/pilot", "date": "2026-10-01" },
      "backup": { "name": "Fire station 17", "recipient": "+919800000017", "channel": "voice" } },
    { "service": "police", "name": "Police station 17", "recipient": "+919800000117", "channel": "sms", "mode": "drill" },
    { "service": "ambulance", "name": "EMS north", "recipient": "+919800000210", "channel": "sms", "mode": "drill" }
  ]
}
```

`gateway/escalation/registry.example.json` is the same shape without comments, with every
service a drill contact. `npm run gateway:cli -- registry check <file>` loads a file the way the
gateway will and prints what it understood, phone numbers masked.

| Field | Meaning |
|-------|---------|
| `version` | 2. Absent means 1. |
| `service` | `fire`, `police`, `ambulance` or `site`. One entry per service; `site` is the warden or security desk. |
| `recipient` | Whatever the channel needs: a phone number for `sms` and `voice`, an endpoint id for `api`. |
| `channel` | `sms`, `voice`, `dashboard`, `api` or `console`. `dashboard` sends nothing: the recipient answers on the gateway's dashboard. `console` exists so escalation can be tested with no account. |
| `mode` | `drill` or `live`; absent means `drill`. A drill contact receives drills only. A live contact also receives real incidents. |
| `agreement` | `{reference, date}`. Required for a live fire, police or ambulance contact: the arrangement that lets the service receive real alerts. The site contact needs none. |
| `backup` | Optional. Used when the primary does not acknowledge inside the window. It shares the contact's mode. |
| `ack_timeout_s` | The acknowledgement window, 60 s unless set (at least 5). |
| `max_realerts` | Re-alerts before the outcome is `escalate`, 1 unless set. |
| `offline_notify_min` | Minutes a unit may be silent before the site is told, 10 unless set. |

## Rules

1. **Every site names all three services** before it goes live. A site with no ambulance
   contact is an incomplete installation, not a working one. The gateway refuses such a file.
2. **Only CRITICAL escalates.** An ELEVATED state notifies the site only, on the dashboard.
3. **The registry is versioned.** When the gateway starts with a registry file whose SHA-256
   differs from the last one recorded, it writes a `registry_change` entry to the site ledger
   with the old and new hashes, so "who was supposed to be called" is answerable months later.
   Keep every version of the file: the hash names it, it does not contain it.
4. **Contacts are verified during commissioning** with a drill that the receiving station
   acknowledges. A service becomes `live` only after that drill, and only with its agreement
   named in the file.
5. **FireX never dials or messages 100, 101, 102, 108, 112, 1091, 1098 or 181.** Automated
   alerts into the public emergency numbers need the service's agreement; until then the
   registry points at station numbers and site contacts that have agreed to receive alerts. The
   gateway refuses a registry that names one of these numbers, however it is written (`+91 112`,
   `0112`, `1-0-1`).

## Who receives what

| Event | The site | A live service | A drill service |
|-------|----------|----------------|-----------------|
| CRITICAL, as it happens | alerted | alerted | not alerted; the site ledger records `escalation_withheld` (`drill_mode`) when no service is live |
| CRITICAL, late | alerted, marked LATE | not alerted | not alerted |
| Drill | alerted if chosen | alerted if chosen | alerted if chosen |
| ELEVATED | dashboard only | no | no |
| A unit silent for `offline_notify_min` | one notice | no | no |

**A late CRITICAL** is one the unit had already written when it last connected, that was over by
the time its backlog reached the gateway, and whose time is either more than 120 s old by a clock
the unit had from the gateway, or untrusted (`ts_src` `uncertain`). The gateway waits up to 10 s
for the backlog before it decides. Services are not called about a fire that, by the unit's own
record, has already ended. The site is told, because someone should look. A backlog CRITICAL
whose trusted time is recent is escalated in full.

**Drills** are started by an admin on the dashboard or with
`npm run gateway:cli -- drill start --services fire,police,ambulance,site`, which queues the drill
for the running gateway. Every drill message begins `DRILL - NOT A REAL INCIDENT`. Drills are
acknowledged, timed out and re-alerted exactly as incidents are, and recorded in `drill_events`
and the site ledger with `drill: true`.

## Messages and acknowledgements

Messages are plain ASCII and fit one SMS (160 characters). The site name, the unit's location
and the hazard are shortened first:

```
FIREX CRITICAL Hostel, Block B: Room 206 gas + heat agree. Reply 1 to acknowledge.
FIREX LATE Hostel, Block B: Room 206 flame. Reported late: unit was offline. Reply 1 to acknowledge.
DRILL - NOT A REAL INCIDENT. FIREX Hostel, Block B: test alert for Fire station 42. Reply 1 to acknowledge.
FireX: unit Room 206 offline since 1 Oct, 14:05
```

An acknowledgement arrives one of three ways:
- an SMS from the recipient's number saying `1`, `OK` or `ACK`, which acknowledges that
  recipient's newest unanswered alert;
- a `1` pressed during a voice alert, where the modem reports key presses;
- an operator pressing Acknowledge on the incident in the dashboard, which acknowledges the site.

Every SMS the gateway receives is kept in `sms_inbox`, matched or not.

## Timeouts

| Setting | Default | Note |
|---------|---------|------|
| Acknowledgement window | 60 s | After this, the backup contact is alerted and the silence is logged. |
| Re-alerts before escalation | 1 | After the re-alert, the outcome is logged as `escalate` and the dashboard raises it to the site. |

Both are per-site settings in the registry. A version 1 file without them uses
`FIREX_ALERT_ACK_TIMEOUT_S` and `FIREX_ALERT_MAX_REALERTS`.

## On the gateway

The gateway loads the registry at start from the file named by `FIREX_SERVICE_REGISTRY`. The
registry's `site_id` must match `FIREX_SITE_ID`. A registry that fails a rule names the field,
and the gateway runs without escalation rather than with an incomplete list.

| Setting | Default | Meaning |
|---------|---------|---------|
| `FIREX_ALERT_PROVIDER` | `off` | `off`, `console` (printed to the gateway log, nothing acknowledges), `http`, or `modem`. |
| `FIREX_ALERT_HTTP_URL` | none | Where `http` sends alerts: mock endpoints for a dry run. |
| `FIREX_MODEM_DEVICE` | `/dev/ttyUSB2` | The GSM modem's AT port. |
| `FIREX_MODEM_BAUD` | 115200 | |
| `FIREX_MODEM_RING_S` | 30 | How long a voice alert rings before the gateway hangs up. |
| `FIREX_ALERT_POLL_MS` | 1000 | How often acknowledgements are checked. A re-alert goes out up to this long after its window ends, never before. |

### The GSM modem

A pilot site uses `modem`: a USB GSM modem (SIM800 or SIM7600 class) on the gateway, with a SIM
on a plan that allows SMS and calls. It needs no internet, no aggregator account and no bulk
sender registration, and it keeps working when the site's uplink does not. One SIM sending a
handful of messages per incident is within an ordinary plan's limits. A site that sends more,
or wants a registered sender name, needs an SMS aggregator and its sender and template
registration, which this gateway does not do.

- The modem provider reaches only phone numbers, over `sms` or `voice`, plus `dashboard`
  contacts. The gateway refuses to start `modem` with an `api` or `console` contact, or with
  a recipient that is not a phone number.
- It runs on Linux only. The tty is configured with `stty` and needs no native module. In
  Docker, uncomment the `devices` and `group_add` lines in `infra/docker-compose.yml`.
- Commands go to the modem one at a time, each with a timeout. A modem that stops answering
  fails that send, which is recorded as `alert_failed` and retried at the window. It does not
  hang the escalation.
- A voice alert rings the number and hangs up. It is placed after any call already ringing,
  so several voice contacts alerted at once are called one after another, 30 s apart.
- Where the modem reports key presses (`+DTMF:` on a SIM800 after `AT+DDET=1`, `+RXDTMF:` on
  some SIM7600 firmware), pressing 1 acknowledges. Otherwise a voice contact acknowledges by
  SMS.

## What is recorded where

| Record | Database | Site ledger (signed) |
|--------|----------|----------------------|
| Alert sent, acknowledged, timed out | `alert_events` (incidents), `drill_events` (drills), with millisecond times | `alert_sent`, `alert_ack`, `alert_timeout`, each naming the unit, the incident, and the unit's CRITICAL entry by `critical_seq` and `critical_hash`, with `drill` true or false |
| Alert that could not be sent | `alert_failed` rows | not recorded |
| Services not alerted | `incidents.escalation_state` and `escalation_reason` | `escalation_withheld`: `verification`, `late` or `drill_mode` |
| Drill started and finished | `drills` | `drill`, phase `started` and `finished` |
| Incident label | `incidents.label` | `incident_label`, naming the CRITICAL entry |
| Registry file changed | none | `registry_change`, with the old and new SHA-256 |
| Unit silent, then back | `heartbeat_gaps` | `heartbeat_gap`, written when the unit returns |
| SMS received | `sms_inbox` | not recorded |

The site ledger is the gateway's own chain, in the same entry format as a unit's and checked by
the same verifier. `npm run gateway:cli -- site-ledger export --out site.json`, then
`npm run verify -- site.json`.

**Restarts.** An escalation or drill in progress when the gateway stops is resumed when it
starts again, from the rows already written, if it began less than an hour before. Older ones
are closed as not resumed. A reply that arrives while the gateway is down is read from the SIM
when it comes back, but it matches an alert only if that escalation was resumed.

**Limits, stated.**
- Heartbeat gaps are measured by the gateway, so the gateway's own downtime shows as a gap for
  every unit.
- A voice alert counts as delivered when the call is placed, not when it is answered.
- An SMS counts as delivered when the modem accepts it (`+CMGS`), not when it reaches the
  handset.

## Dry runs

`npm run escalation:dry-run` times the alert path with no hardware. It runs simulated units, an
in-process broker, the gateway's ingest and escalation, and mock endpoints that answer after a
delay or stay silent. Every service is treated as live for the dry run. It writes:
- the latency distribution from threshold crossing to alert delivered;
- per-service acknowledgement times;
- the observed re-alert delay.

It fails if an alert is lost or a re-alert fires early.

On the bench, run `npm run mock:endpoints -- --port 9090 --log endpoints.jsonl` beside the
gateway, with `FIREX_ALERT_PROVIDER=http` and `FIREX_ALERT_HTTP_URL=http://127.0.0.1:9090`. Every
delivery and acknowledgement lands in the log with the unit's device ID, the CRITICAL entry's
sequence number and the time the gateway stored it, for joining with a capture of the unit.
