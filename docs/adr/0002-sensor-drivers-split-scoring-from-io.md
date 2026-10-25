# ADR 0002 — Sensor drivers split pure scoring from ESP-IDF I/O

Date: 2026-09-15
Status: accepted

## Context

M1 brings up one driver per sensor behind a common score API. Each driver has two very
different halves: talking to hardware (I2C, ADC, RMT, PWM) and turning readings into a
0-1 hazard score with a validity flag. The second half is where the product's judgement
lives: Rs/R0 curves, flicker detection, rate of rise, what counts as a failed sensor.
It is also the half that the M4 replay harness and the M6 PCB port both need unchanged.

## Decision

`firmware/components/sensors/` has two directories:

- `score/` — pure C11, no ESP-IDF includes. ADS1115 register arithmetic, MQ scoring and
  baseline capture, flame flicker scoring, DHT22 frame decoding, heat and humidity
  scoring, fan health, and the `fx_sensor_t` collector. Built and tested on the host by
  `firmware/Makefile` (`test/test_sensors.c`), exactly like the fusion engine.
- `hw/` — thin ESP-IDF drivers that do the I/O in their own FreeRTOS task and hand raw
  values to `score/`. `score()` reads cached results under a spinlock and never blocks
  on I/O.

`fx_sensor_t` is a small vtable (`init`, `poll`, `score`, `raw`) plus the bitmask of
modalities a driver owns. The collector starts every modality invalid and copies only
owned modalities, so a missing or failed driver can only ever produce "invalid".

## Consequences

- Scoring logic is covered in CI with no board, and a recorded trace can be replayed
  through the same functions on a laptop in M4.
- The M6 PCB swaps files in `hw/` (production sensors, a fast thermistor) and keeps
  `score/` and the fusion engine.
- Two files per sensor instead of one. Accepted: the boundary is the point.
- Hardware behaviour — timing, bus errors, electrical noise — is not covered by the host
  tests and still needs the bench checklist in `docs/bringup/`.
