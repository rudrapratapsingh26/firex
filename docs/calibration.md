# Calibration procedure

Status: M2, written before the first calibration has been done. Revise it after the first
run, and record every revision in the table at the end.

This is the procedure that turns the dev kit's default numbers into calibrated
thresholds (the M2 milestone). It is a document so that the second calibration matches the
first, and so that anyone reading an exported log can see how the thresholds it ran on
were chosen. Every value it produces is entered on the node, where it becomes a
`calibration` or `config_change` ledger entry.

## 1. When to calibrate

- Before first use, after the 48-hour MQ burn-in.
- After replacing any sensor.
- Every 30 days, and after any self-test that fails the `heater` check.

## 2. Conditions

| Condition | Requirement | How it is checked |
|-----------|-------------|-------------------|
| Burn-in | New MQ sensors powered for 48 h | Uptime from `status`, or a note on the record sheet |
| Warm-up | Node powered at least `fx_warmup_s` | The console refuses `baseline` sooner |
| Air | Ventilated room, no cooking, cleaning products, incense or smoke for 2 h | Operator |
| Ambient | 20-30 °C, 40-60 % RH, recorded from `read` | DHT22 reading on the sheet |
| Fan | Running at the duty it will run at in service (`config get fan_duty`) | `read` |
| Firmware | The version to be used in service | `status` |

## 3. Record the starting point

1. `status`, `config list` and `ledger status`: save the output with the date.
2. `ledger export` with `tools/ledger/export_log.py`, so the log before calibration is kept.

## 4. Warm-up time

1. Power the node from cold and record 30 minutes of clean air:
   `python tools/capture/capture.py --port COMx --out captures/cal-warmup-<date> --period 2000`
2. Run `tools/calibration/01-warmup-curves.ipynb` on the capture.
3. Repeat on two more days. Take the worst settle time, round up to the next 30 s, and set
   `config set fx_warmup_s <seconds>`.

## 5. Clean-air baselines (R0)

1. After warm-up, start a 10-minute capture as in step 4.
2. On the console: `baseline mq2 60`, wait for the result line, then `baseline mq135 60`.
3. Accept a baseline only if its spread is under 5 %. An unstable baseline means the air
   or the heater is not settled: wait 30 minutes and repeat.
4. Run `tools/calibration/02-clean-air-baselines.ipynb` on the capture. The notebook's R0
   and the node's must agree within 5 %. If they do not, find out why before continuing.

The node stores each accepted R0 and writes a `calibration` entry (method `clean_air_median`).

## 6. Gas and smoke curves

1. Run the fan-response procedure from `docs/bringup/README.md` with the fan at its service
   duty, at least five exposures.
2. From the capture, read the lowest Rs/R0 the MQ-2 reached at the reference dose.
3. Set `mq2_full` (hundredths) so that the reference dose scores about 0.8. Repeat for the
   MQ-135 with its reference smoke source and `mq135_full`.

## 7. Flame thresholds

1. `stream on 100` and capture while you present, for 20 s each: a dark room, room
   lighting, a phone torch waved in front of the sensor, sunlight through a window, a
   lighter flame at 1 m, and a lighter flame at 50 cm.
2. For each, read the `flame_band_rms` column.
3. Set `flame_quiet` to 1.5 times the highest non-flame RMS, and `flame_full` to the RMS of
   the flame at 1 m. If the flame at 1 m does not reach twice `flame_quiet`, the sensor
   placement is wrong for this room; fix that rather than the numbers.

## 8. Heat

On the dev kit the DHT22 gives the temperature. On the EVT board (M6) a thermistor on the
ADS1115 gives it, and the DHT22 gives humidity only: `status` lists both `ntc` and `dht22`,
and the capture has `temp_c` from the thermistor and `dht_temp_c` from the DHT22.

1. **EVT board only, before anything else.** With the power off, measure the thermistor
   divider's fixed resistor. With the node powered, measure the divider supply at the top of
   that resistor. If either differs from `FIREX_NTC_FIXED_OHMS` or `FIREX_NTC_SUPPLY_MV` by
   more than 1 %, rebuild the image with the measured values: they are build settings, not
   `config` keys. A supply 1 % off reads about 0.45 °C wrong at 25 °C.
2. Compare `read` against a reference thermometer next to the heat sensor for 10 minutes.
   The difference must stay within 1 °C.
   - DHT22: if not, replace the sensor.
   - Thermistor: if not, recheck step 1, then the part's B value against its datasheet
     (`FIREX_NTC_BETA`, `FIREX_NTC_R25_OHMS`). The firmware uses the Beta equation, which
     departs from a part's own table away from 25 °C, so also compare at a second
     temperature at least 15 °C from the first (a warm room, or the reference and the unit
     together in front of a fan heater on low) and record both differences.
   - A thermistor that reads `open`, `short` or `out_of_range` in `ntc_status` is a wiring
     or part fault. Heat stays invalid until it is fixed; do not calibrate around it.
3. **Thermistor response (EVT).** Point a hair dryer on low at the unit from 50 cm and start a
   capture with `stream on 500`. Record how many seconds after the dryer starts `ror_c_min`
   first passes `ror_start`. This is the number the thermistor was chosen to shorten: keep it
   with the record sheet, next to the same measurement on a dev-kit unit if one is available.
4. Leave `heat_start_c`, `heat_full_c`, `ror_start` and `ror_full` at their defaults unless
   the site's normal ceiling temperature is within 10 °C of `heat_start_c`.

## 9. Check the result

Run each with the node in NORMAL, and a second person present:

| Test | Expected | Fails the calibration if |
|------|----------|--------------------------|
| Kettle steam at 1 m for 2 min | NORMAL, or ELEVATED on gas only | CRITICAL |
| Toast lightly browned 2 m away | at most ELEVATED | CRITICAL |
| Phone torch waved 30 s | NORMAL | flame score above `fx_enter` |
| Reference gas dose at 30 cm and a hair dryer on the heat sensor (the DHT22 on a dev kit, the thermistor on an EVT board) | CRITICAL within 60 s | no CRITICAL |
| Lighter flame at 50 cm for 5 s | CRITICAL (flame alone) | no CRITICAL |

Aerosols are not in this check, and a pass here says nothing about them. With `fx_mos_pair`
at its default of 1, gas and smoke agreeing for `fx_crit_ms` is CRITICAL, and a deodorant
spray can drive both metal-oxide sensors. M4 measures that against smouldering fires before
the setting is changed (`docs/validation.md` §3 and §5).

Then run `selftest` and confirm PASS.

## 10. Close

1. `ledger tail 30` shows the `calibration` and `config_change` entries for every change.
2. Export the log and verify it: `npm run verify -- <bundle>.json`.
3. Fill in the record sheet and keep it with the exported bundle.

## Record sheet

| Field | Value |
|-------|-------|
| Date, operator, second person | |
| Node device_id and firmware version | |
| Ambient temperature, RH | |
| Warm-up settle times (three days), `fx_warmup_s` set | |
| MQ-2 R0 (node / notebook), spread | |
| MQ-135 R0 (node / notebook), spread | |
| `mq2_full`, `mq135_full` | |
| Flame RMS: dark / room / torch / sun / flame 1 m / flame 50 cm | |
| `flame_quiet`, `flame_full` | |
| Heat sensor (DHT22 or thermistor) against reference, max difference; second point (thermistor) | |
| EVT: divider resistor and supply measured, values in the image | |
| EVT: seconds from hair dryer on to `ror_c_min` above `ror_start` | |
| Section 9 results | |
| Self-test result | |
| Exported bundle file and verify result | |

## Revisions

| Date | Change | Why |
|------|--------|-----|
| 2026-09-15 | First version | M2 |
| 2026-09-15 | Note on aerosols and `fx_mos_pair` in §9 | M4: gas and smoke are one sensing physics |
| 2026-09-16 | §8 and the record sheet cover the EVT thermistor | M6: the thermistor replaces the DHT22 for heat |
