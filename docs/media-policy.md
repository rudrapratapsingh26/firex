# Media policy

Status: draft, M0. Written before the camera code, because a camera in a hostel or a
hospital ward is a privacy liability before it is a feature. Every rule here is
enforced in firmware or on the gateway, not in a configuration file a site can change.

Images of identifiable people are personal data. FireX handles them as evidence of one
incident, not as a video system.

## What the node does

| Rule | Enforced where |
|------|----------------|
| The camera arms **only** on the CRITICAL transition. There is no live feed, no continuous recording, and no remote shutter. | Node firmware |
| One incident captures a fixed number of frames and one short clip, then the camera disarms. | Node firmware |
| Every frame is hashed with SHA-256 and the hashes go into a `media_captured` ledger entry. | Node firmware |
| The camera task never delays the alarm task. Frames are dropped before an alarm is delayed. | Node firmware, task priority |
| A camera that fails its daily self-test raises a fault; it never silently stops working. | Node firmware |

## What the gateway does

| Rule | Enforced where |
|------|----------------|
| Images are stored on the site gateway, on the site's own LAN. | Gateway |
| Images are never sent to the anchor. Only the 32-byte Merkle root of the log leaves the building. | Merkle builder |
| Emergency services fetch an incident's media through a time-limited link, and each access is logged. | Gateway API |
| Media is deleted at the end of the retention window, and the deletion is itself a logged event. | Gateway retention job |
| Nothing is uploaded on a schedule. Media moves only because an incident happened. | Gateway |

## Retention

- Default retention: **30 days** for incident media, then deletion.
- The ledger entries that reference the media are **never** deleted; a hash outlives the image.
- A site may shorten the window. Lengthening it needs the site's written instruction,
  recorded as a `config_change` entry.

## What this policy does not cover yet

- Consent notices at the door of a monitored room. A pilot host will want these; the
  wording is theirs, not ours.
- Whether the ambulance service should receive images at all, or only the fire service.
  Open question for the M5 pilot.
- Face blurring. Not planned: the camera looks at a ceiling-height view of a room during
  a fire, and blurring would cost the detail a crew needs.

## Why the hash matters

A photo attached to an incident report is an attachment. A photo whose SHA-256 appears in
a signed, hash-chained entry is evidence: re-hash the file and it either matches the chain
or it does not. That is the difference FireX is selling, and it applies to images exactly
as it applies to events.
