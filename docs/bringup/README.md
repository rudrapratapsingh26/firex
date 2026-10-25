# M1 bring-up: dev kit, wiring, procedures

Status: written before any of it has run on a board. Pin numbers, resistor values and
every threshold are starting points; correct this page when the bench disagrees.

M1 milestone (Software Roadmap v2.0): **all five hazard signals reading on one board, a
photo captured on demand, and a measured fan-on versus fan-off response curve.**

## 1. Parts

| Part | Role | Firmware |
|------|------|----------|
| ESP32-S3-DevKitC-1 (N8R8) | the node | `firmware/` |
| ADS1115 breakout | 16-bit ADC for both MQ sensors | `components/sensors/hw/ads1115.c` |
| MQ-2 breakout | gas | `hw/mq_driver.c`, `score/mq_score.c` |
| MQ-135 breakout | smoke / air quality | same driver, second instance |
| KY-026 IR flame module | flame (flicker, 1-15 Hz) | `hw/flame_driver.c`, `score/flame_score.c` |
| DHT22 (AM2302) | heat and humidity | `hw/dht22_driver.c`, `score/dht22_frame.c`, `score/heat_score.c` |
| 5 V DC fan, logic-level N-MOSFET, diode | airflow past the MQ sensors | `hw/fan.c`, `score/fan_health.c` |
| AI-Thinker ESP32-CAM + ESP32-CAM-MB | camera, separate MCU | `firmware/cam/` |
| 5 V 2 A supply | heaters and fan | |

## 2. Wiring (defaults in `idf.py menuconfig` → FireX node → Sensors)

| From | To | Notes |
|------|----|-------|
| ADS1115 VDD / GND | 3V3 / GND | Run it at 3.3 V so its I2C levels match the ESP32-S3. |
| ADS1115 SDA / SCL | GPIO 8 / GPIO 9 | Most breakouts carry 10 k pull-ups. |
| ADS1115 ADDR | GND | Address 0x48. |
| MQ-2 VCC / GND | 5 V supply / GND | Heater ~150 mA. |
| MQ-2 AO | 10 k → ADS1115 A0, A0 → 20 k → GND | AO swings to 5 V; the divider keeps A0 under 3.3 V. |
| MQ-135 | same as MQ-2, on A1 | |
| KY-026 VCC / GND | 3V3 / GND | Powered at 3.3 V so AO can go straight to the ESP32 ADC. |
| KY-026 AO | GPIO 4 (ADC1) | DO is unused. |
| DHT22 VCC / GND | 3V3 / GND | |
| DHT22 DATA | GPIO 5, 10 k pull-up to 3V3 | Many modules already have the pull-up. |
| Fan − | MOSFET drain; source to GND | Diode across the fan, cathode to +5 V. |
| MOSFET gate | GPIO 6 through 100 Ω, 10 k to GND | 25 kHz PWM. |
| Fan tachometer (3-wire fans) | GPIO 7, 10 k pull-up to **3V3 only** | Reads reliably only at duty 0 or 100. |
| ESP32-CAM | its own MB adapter and USB cable | Not wired to the node in M1. |

**Power.** Two heaters, a fan and the ESP32-S3 draw more than a laptop USB port
supplies. Feed the 5 V rail from the 2 A supply and join its ground to the dev kit's.

**Measure before trusting defaults.** Read RL on each MQ breakout with a multimeter
(power off) and set `FIREX_MQ2_RL_OHMS` / `FIREX_MQ135_RL_OHMS`. Boards ship with anything
from 1 k to 10 k. A wrong RL scales Rs, but R0 is captured through the same RL, so the
ratio survives; the absolute Rs in the notebooks does not.

## 3. Build and flash

```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor          # the "UART" USB port on the dev kit
```

On boot the node prints a banner and a `boot` ledger entry; a sensor that did not come up
shows as a `fault` entry about 15 s later. (The M2 additions to boot are in
[m2-bench.md](m2-bench.md).) Then at the `firex>` prompt:

```
i2cscan                 # expect 0x48
status                  # every sensor "ready"
read                    # scores and raw values
```

Gas and smoke stay INVALID until they have an R0: invalid, never zero, by design.

```bash
cd firmware/cam
idf.py set-target esp32
idf.py menuconfig                   # FireX camera -> BENCH ONLY: allow SNAP from the USB console
idf.py build flash                  # the MB adapter holds IO0 low for flashing
python ../../tools/camera/pull_jpeg.py --port COMx
```

From M2 the camera image refuses capture requests on its USB console unless it is built
with that bench option (see docs/media-policy.md). Rebuild without it before connecting the
camera to the node.

## 4. Procedures

Every capture uses `tools/capture/capture.py`, which writes a CSV and an events file
into `captures/` (git-ignored). Analyse them with the notebooks in `tools/calibration/`.

| # | Procedure | Capture | Notebook |
|---|-----------|---------|----------|
| 1 | Cold-start warm-up, three separate days | 30 min clean air from power-on, `--period 2000` | `01-warmup-curves` |
| 2 | Clean-air baselines, after warm-up | 10 min clean air; `baseline mq2`, `baseline mq135` on the console | `02-clean-air-baselines` |
| 3 | RH sensitivity | clean air while humidity changes | `04-rh-sensitivity` |
| 4 | Fan on vs fan off | ≥ 5 alternating runs each, fixed dose and distance, `gas in` / `gas out` marks | `03-fan-response` |
| 5 | First JPEG | `pull_jpeg.py`; the saved file's SHA-256 matches its sidecar | — |
| 6 | Flame flicker | lighter flame at 50 cm, then a phone torch and sunlight, `stream on 100` | set `flame_quiet` / `flame_full` with `config set` (docs/calibration.md §7) |

Safety for 4 and 6: small doses only, in a ventilated space, nothing else lit, a
second person present, and never near the MQ heaters with an open flame and a gas
release at the same time.

## 5. M1 exit checklist

- [ ] Node image builds, flashes, and `status` shows every sensor ready.
- [ ] `read` shows gas, smoke/AQ, flame, heat and humidity all valid at the same time.
- [ ] Warm-up settle time recorded for both MQ sensors (worst of three days).
- [ ] R0 recorded for both MQ sensors, with spread under 5 %.
- [ ] RH sensitivity recorded for both MQ sensors.
- [ ] Fan-on versus fan-off t90 medians recorded, with run counts.
- [ ] A JPEG pulled over serial, its SHA-256 matching the frame header.
- [ ] Flame scorer thresholds set from real ADC counts.

## 6. What is not verified yet

- Nothing in `firmware/main`, `components/sensors/hw` or `firmware/cam` has run on a
  board. The pure-C scoring under `components/sensors/score` is host-tested; the I/O
  around it is compiled, not exercised.
- The DHT22 read relies on the RMT receiver capturing the frame after the start pulse is
  released. If a real part returns `short_frame`, the start-pulse length and the order of
  arming and release in `hw/dht22_driver.c` are the first things to check.
- The flame scorer's RMS thresholds are placeholders in ADC counts.
- All MQ scoring constants (`ratio_full`) and heat constants are starting points from
  datasheets and standards, not calibration.
