# Site service registry

Status: draft, M0. Defines how a site is mapped to the emergency services that cover it.

The mapping is made **at installation and stored on the site gateway**. Nothing is looked
up during an alarm: a node that has just confirmed a fire is not the moment to be
resolving a jurisdiction.

## Shape

```jsonc
{
  "site_id": "site-001",
  "site_name": "Hostel, Block B",
  "address": "Block B, example campus",
  "location": { "lat": 0.0, "lon": 0.0 },
  "contacts": [
    { "service": "site",      "name": "Warden on duty",    "recipient": "warden-1",   "channel": "sms" },
    { "service": "fire",      "name": "Fire station 42",   "recipient": "station-042","channel": "sms",
      "backup": { "name": "Fire station 17", "recipient": "station-017", "channel": "voice" } },
    { "service": "police",    "name": "Police station 17", "recipient": "ps-017",     "channel": "sms" },
    { "service": "ambulance", "name": "EMS north",         "recipient": "ems-north",  "channel": "api" }
  ]
}
```

| Field | Meaning |
|-------|---------|
| `service` | `fire`, `police`, `ambulance` or `site`. One entry per service; `site` is the warden or security desk. |
| `recipient` | Whatever the channel needs: a phone number, a station code, an API endpoint id. |
| `channel` | `api`, `sms`, `voice` or `console`. The console channel exists so escalation can be tested with no account. |
| `backup` | Optional. Used when the primary does not acknowledge inside the timeout. |

## Rules

1. **Every site names all three services** before it goes live. A site with no ambulance
   contact is an incomplete installation, not a working one.
2. **Only CRITICAL escalates.** An ELEVATED state notifies the site only.
3. **The registry is versioned.** Every change is a `config_change` ledger entry carrying
   the old and new values, so "who was supposed to be called" is answerable months later.
4. **Contacts are verified during commissioning** with a test alert that the receiving
   station acknowledges. An unverified contact is flagged on the dashboard.
5. **FireX never auto-dials 100, 101, 108 or 112.** Automated calls into the public
   emergency numbers need the service's agreement; until then the registry points at
   station numbers and site contacts that have agreed to receive alerts.

## Timeouts

| Setting | Default | Note |
|---------|---------|------|
| Acknowledgement window | 60 s | After this, the backup contact is alerted and the silence is logged. |
| Re-alerts before escalation | 1 | After the re-alert, the outcome is logged as `escalate` and the dashboard raises it to the site. |

Both are per-site settings, changed through the registry, and both changes are logged.
