# Media policy

Status: draft, M0; enforcement tables added at M2 and M3. Written before the camera code, because a camera in a hostel or a
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

## How the firmware enforces it (M2)

| Rule | Where |
|------|-------|
| Capture happens only on the transition into CRITICAL. No console command, setting or other code path sends a capture request. | `firmware/main/camera_task.c`, woken only by `fusion_task.c` |
| At most `cam_frames` frames per incident (0-5, default 3), at most one incident a minute, then the camera task sleeps again. | `camera_task.c` |
| Every frame is hashed by the node as it arrives and compared with the camera's hash; the node's hashes go into `media_captured`. | `components/media/src/frame_rx.c` |
| The camera task runs at low priority. A silent camera or a full media partition drops frames; nothing waits on them. | `camera_task.c`, task priorities in `node.h` |
| The daily self-test uses `CHECK`: the camera captures a frame and reports only its size. No image leaves the camera. | `firmware/cam/main/cam_main.c`, `selftest_task.c` |
| A camera that fails its self-test raises a fault until a later self-test passes. | `selftest_task.c` |
| The camera answers capture requests only on its link to the node. On its USB console, capture is refused. | `cam_main.c` |

### Bench exception

`tools/camera/pull_jpeg.py` needs the camera to accept a capture request on its USB
console. That is a remote shutter, so it exists only in a camera image built with
`CONFIG_FIREX_CAM_BENCH_SHUTTER`, which is off by default and makes every banner and
`FXPONG` reply say `bench`. A bench image is for a test scene on a developer's bench and
is never fitted to a unit that can see people.

The node has a second bench exception, from M4. `CONFIG_FIREX_BENCH_INJECT` adds
`inject flame <seconds>` to the node's console. It flickers the self-test IR LED at the
flame sensor until fusion raises CRITICAL, and that CRITICAL captures frames like any other.
No capture request is sent from the console, but the effect is a shutter, so the same rule
applies:
- the option is off by default;
- the firmware version ends in `+inject`, in the banner, the `boot` entry and on the gateway;
- `status` prints a warning;
- the image is used only for the burn-in and camera checks on a test scene
  (`docs/validation.md` §7 and §8).

Physical access to the wires between the node and the camera is equivalent to a shutter.
On the M6 EVT board the link runs inside the enclosure. The camera module on a production unit
runs only FireX's signed image, so its refusal of capture requests on its console cannot be
flashed away (see "Production units (M6)" below).

## How the gateway enforces it (M3)

| Rule | Where |
|------|-------|
| A photo travels on its own MQTT topic named by its SHA-256. The gateway refuses bytes that do not hash to the topic, and anything over 2 MiB. | `gateway/server/ingest.ts` |
| The node deletes its copy only after the gateway has stored the photo and checked its hash. Until then the photo waits on the node's media partition. | `firmware/main/net_task.c` |
| A unit can publish photos only under its own device ID: the broker takes the unit's identity from its client certificate. | `infra/mosquitto/acl` |
| Only signed-in accounts see photos. Every view of a photo and every log export is written to the audit log with the account and the time. | `gateway/server/http.ts` |
| Photos are deleted when the retention window ends, 30 days by default. Each deletion is written to the audit log; the ledger entry and its hash stay, and the dashboard says the photo was deleted. | `gateway/server/retention.ts` |
| Photos stay on the gateway's disk, on the site LAN. Nothing in M3 sends them anywhere else. | `infra/docker-compose.yml` |

Not enforced yet, and stated so nobody assumes otherwise:

- Time-limited links for emergency services arrive with escalation. Until then only site
  accounts can open a photo.
- The retention window is an environment variable on the gateway. Lengthening it is not
  yet recorded as a `config_change` entry, so a longer window rests on the site's written
  instruction alone.

## Pilot sites (M5)

| Rule | Where |
|------|-------|
| Alerts to the site contact and to services are text only: an SMS names the site, the room and the hazard. No photo and no link to one leaves the site by SMS or voice. | `gateway/escalation` |
| A pilot export carries no photos. It holds the photos' hashes in the unit bundles; the images stay on the gateway under the retention window, and an inquiry fetches them there. | `gateway/server/pilot.ts` |
| Episode traces are scores, fan and alarm states sampled every 500 ms around an episode. They contain no image, sound or identity, travel like photos (hashed into the ledger, deleted from the unit once the gateway has them), and are part of the pilot export. | `firmware/main/trace_task.c`, `docs/transport.md` §7 |
| The bench injection image (`+inject`) is never installed at a pilot site. | `docs/pilot.md` §4 |

Time-limited links for emergency services are still not built: in M5 services receive text,
and only site accounts on the site LAN can open a photo.

## Production units (M6)

The rules above that were still settings or bench options are compiled into a production image
(`sdkconfig.production`, [ADR 0009](adr/0009-secure-boot-and-production-locks.md)). A site
cannot change them, and neither can anyone with a USB cable.

| Rule | Where |
|------|-------|
| The number of frames per CRITICAL is fixed at build time (`CONFIG_FIREX_CAM_FRAMES_LOCKED`, 3 by default). `config set cam_frames` is refused. | `firmware/components/core` settings, `firmware/main/camera_task.c` |
| There is no `media get`: a photo leaves the unit only to its gateway, over mutual TLS. `media list` shows names and hashes. | `firmware/main/console.c` |
| Neither bench option exists: `inject flame` cannot be selected, nor the camera's console shutter, and the build refuses a production image that has either. | `firmware/main/Kconfig.projbuild`, `firmware/cam/main/Kconfig.projbuild`, `firmware/main/node_main.c` |
| Only firmware signed with the FireX release key boots on the node or the camera. Flash is encrypted, JTAG is off, and the node's ROM loader cannot read flash. | Secure boot v2 and flash encryption, burned at first boot |
| The camera's ROM loader is disabled outright, so no unsigned code runs on it, even in RAM. | `firmware/cam/sdkconfig.production` |
| A production unit takes an update only if the image is itself a production image: its kind, inside the signed image, must be `evt-production`. A signed dev kit or EVT development image, which has none of these locks, is refused. | `firmware/main/image_kind.c`, `firmware/main/ota_task.c`, `firmware/components/ota` |

The dev kit and the EVT development image keep the M2-M5 behaviour.

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
