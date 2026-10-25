# M2 bench: alarm, ledger and camera trigger

Status: written before any of it has run on a board. The firmware builds against ESP-IDF
5.5.5 and its hardware-independent logic passes 100 host tests; the hardware paths below
are unexercised.

M2 milestone (Software Roadmap v2.0): **calibrated thresholds and a working alarm state
machine on the bench, with a CRITICAL entry that carries a verifiable image hash.**

Start from a node that passes the M1 checklist in [README.md](README.md).

## 1. Additional parts and wiring

Defaults are in `idf.py menuconfig` → FireX node → Alarm and self-test, and Camera link.

| From | To | Notes |
|------|----|-------|
| Buzzer + | GPIO 16 through an NPN transistor or logic-level MOSFET, buzzer − to GND | Active buzzer by default. A bare piezo needs `FIREX_BUZZER_PASSIVE`. |
| WS2812 12-LED ring DIN | GPIO 15 through 330 Ω | Ring 5 V from the supply, grounds joined. |
| Silence button | GPIO 14 to GND | Internal pull-up. The only way to silence the alarm. |
| IR LED (850-940 nm) facing the KY-026, 2 cm away | GPIO 13 through 150 Ω | For the self-test flame loopback. Optional: without it the check is skipped. |
| Node GPIO 17 (TX) | ESP32-CAM GPIO 13 (link RX) | 3.3 V logic both sides. |
| Node GPIO 18 (RX) | ESP32-CAM GPIO 14 (link TX) | |
| ESP32-CAM 5 V / GND | the 5 V supply / common GND | The camera draws 250-300 mA while capturing. |

Leave the ESP32-CAM's SD slot empty: GPIO 13 and 14 are the SD lines.

## 2. Flash

```bash
cd firmware && idf.py set-target esp32s3 && idf.py build flash monitor
cd firmware/cam && idf.py set-target esp32 && idf.py build flash
```

The camera image built with defaults is **link-only**: it takes photos only when the node
asks over the link. `pull_jpeg.py` from M1 needs a bench build (section 5 of the M1 guide).

## 3. First boot

Expect, in order: the banner with boot count and reset reason; on the very first boot a
warning that a new device key was generated; a `boot` entry echoed as an E line; the ring
amber-pulsing (WARMUP) for `fx_warmup_s`, then dim green (NORMAL). Then:

```
status            # faults all "no", ledger open, camera link ok
cam ping          # "camera answered"
ledger key        # the device public key: keep a copy with the unit's records
ledger tail       # boot, fault entries for any sensor not yet wired, WARMUP -> NORMAL
selftest          # after warm-up: PASS, and a self_test entry
```

Any `YES` under faults shows the ring blinking yellow; `ledger tail` says which source.

## 4. Calibrate

Follow [../calibration.md](../calibration.md) completely. The milestone asks for calibrated
thresholds, and the demonstration below is only meaningful on them.

## 5. The milestone demonstration

Two people, ventilated room, nothing else lit, fire extinguisher or blanket at hand.

1. `stream on 500` in a capture window (`tools/capture/capture.py ... `) so the run is recorded.
2. Hold a lighter flame 50 cm from the KY-026 for 5 s. Expect CRITICAL on flame alone: the
   temporal-three buzzer (three half-second tones, 1.5 s pause) and a red ring. Remove the
   flame.
3. While CRITICAL, press the silence button once. Expect SILENCED: buzzer stops, ring stays
   red. If the node falls back below CRITICAL, silence ends by itself.
4. Repeat step 2 to produce a second CRITICAL a minute later if the first did not capture.
5. `ledger tail 20` must show, in order: `threshold_cross` (flame up), `state_change`
   NORMAL → CRITICAL with reason `["flame"]`, `media_captured` with `cam_frames` frame hashes,
   `silence` with `seconds` and `by: button`, and the transitions back.
6. Export and verify:

   ```bash
   python tools/ledger/export_log.py --port COMx --out captures/m2-demo --media
   npm run verify -- captures/m2-demo.json --media captures/m2-demo-media
   ```

   Expect `PASS`. The verifier has checked every hash link, every signature against the
   node's key, and that the frame hashes in `media_captured` match the exported JPEGs.
7. Tamper check: open one JPEG in `captures/m2-demo-media`, change one byte, verify again.
   Expect `TAMPERED ... (media)`. Restore it, then change one character in the
   `state_change` entry in `m2-demo.json` and expect `TAMPERED ... (chain)`.
8. Power-cycle the node, wait for NORMAL, export again: the chain continues from the
   previous `seq` and verifies, with a `boot` entry whose `reset_reason` is `power_on`.

## 6. M2 exit checklist

- [ ] Calibration done and recorded per `docs/calibration.md`, every change present in the ledger.
- [ ] WARMUP, NORMAL, ELEVATED, CRITICAL, SILENCED and FAULT each observed on the bench,
      with the ring and buzzer behaving as `components/alarm/include/fx_alarm.h` describes.
- [ ] The alarm sounds with the camera unplugged, and with the ledger partition full.
- [ ] A task watchdog reset (induced on a debug build) produces a `boot` entry with `reset_reason: watchdog`.
- [ ] `selftest` passes every check that is wired, including `camera` and `flame_loopback`.
- [ ] The section 5 export verifies with `--media`, and both tamper checks fail as expected.
- [ ] The chain continues across a power cycle.
- [ ] `ledger check sig` passes on the node.

## 7. Not verified yet

- The ring refresh blocks the alarm task for roughly 9 ms when the colour changes. Measure the
  worst-case alarm loop time on the bench with a logic analyser on the buzzer GPIO.
- Sustained LittleFS behaviour at the rotation boundary has only been tested with the RAM
  store on the host; run a soak that writes several thousand entries and rotates.
- The camera link at 460800 baud over bench wires: if frames fail their hash, drop
  `FIREX_CAM_BAUD` and the camera's `FIREX_CAM_LINK_BAUD` together.
- Key generation entropy relies on the SAR ADC source being enabled before any ADC driver; that
  ordering is in `node_main.c` and must survive future changes.
