# ADR 0003 — The ESP32-CAM is a second MCU behind a framed serial protocol

Date: 2026-09-15
Status: accepted for M1-M5; kept for the M6 EVT board (see "M6" below)

## Context

The node gains a camera (Software Roadmap v2.0). The ESP32-S3 dev kit has no camera
connector wired, and the AI-Thinker ESP32-CAM is a cheap, complete ESP32 + OV2640 +
PSRAM board. Media policy requires that capture never delays the alarm task and that
every frame is hashed.

## Decision

- The camera runs its own firmware (`firmware/cam/`, target esp32) using Espressif's
  `esp32-camera` component.
- It speaks a line-framed protocol: `SNAP` in; `FXJPEG 1 <length> <sha256> <w> <h>`,
  the raw bytes, then `FXEND` out; `FXERR <reason>` on failure. The camera computes the
  SHA-256 over the exact bytes it sends.
- In M1 the laptop speaks it over USB (`tools/camera/pull_jpeg.py`). From M2 the node
  speaks it over a UART, triggered by the CRITICAL transition, and writes the header's
  hash into the `media_captured` ledger entry.

## Consequences

- A camera crash, a slow JPEG encode or a PSRAM fault cannot stall the node's alarm or
  sensor tasks: they are on a different chip.
- The hash is computed where the image is produced, so the same value is checked by the
  laptop tool today and the ledger later.
- The M1 image accepts `SNAP` from anyone on the serial link, which is a remote shutter.
  That breaks the media policy on purpose and only on the bench; see the policy's
  "M1 bench exception". The command is removed when capture moves behind CRITICAL in M2.
- Two images to build, flash and version. CI builds both.
- The M6 PCB may put a camera sensor on the main MCU. The protocol then becomes an
  internal queue with the same frame fields.

## M6

Revisited for the EVT board and kept. The ESP32-CAM module sits on the PCB as a second MCU,
with the same UART protocol and camera image. The link runs inside the enclosure, and a camera
fault still cannot touch the alarm MCU. Putting the sensor on the ESP32-S3's camera port was
rejected: it would cost S3 pins and PSRAM next to the alarm path.

Because the camera carries its own radio and enforces the console-shutter refusal, it gets a
production image with secure boot v2, flash encryption and a disabled ROM loader. That image
cannot be changed once burned
([ADR 0009](0009-secure-boot-and-production-locks.md)).
