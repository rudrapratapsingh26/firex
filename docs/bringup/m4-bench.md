# M4 bench: error rates, burn-in and the alert path

Status: written before any of it has run on hardware. The replay harness, burn-in analysis,
camera metrics, escalation dry run and report generator have run only on synthetic data.
The bench-injection firmware compiles but has not run on a board.

M4 milestone (Software Roadmap v2.0): **a reliability report with measured error rates, plus
an alert-path latency distribution from threshold crossing to alert delivered.**

Start from a unit that passes the M3 checklist in [m3-bench.md](m3-bench.md), calibrated per
[../calibration.md](../calibration.md). The procedures, metric definitions and pass criteria
are in [../validation.md](../validation.md); this guide is the order to do them in.

## 1. What you need

| Item | Notes |
|------|-------|
| The M3 unit, calibrated | Record `config list` before every session. |
| A second build of the node firmware with injection | Section 3. Only for the burn-in, camera and alert-path sessions. |
| The gateway | On the laptop for the alert-path session, so capture, gateway and mock endpoints share a clock. |
| Nuisance sources | Toaster, hot plate and oil, kettle, incense and a mosquito coil, deodorant and insect spray, a broom, a halogen lamp, a phone torch. |
| Exposure kit | Butane lighter, candle, metal tin with cotton or paper, hair dryer, the calibration reference gas dose. |
| Safety | Fire extinguisher or blanket, a ventilated room, a second person for every exposure. |
| A sound-deadening box for the buzzer | For the 72-hour burn-in. |
| Logic analyser (optional) | CRITICAL to buzzer GPIO timing, as in the M2 guide. |

## 2. Rehearse on a laptop first

Every tool runs on synthetic data, so the whole report can be produced before any trial.
Its SYNTHETIC banner is the point: this shows the pipeline, not the product.

```bash
make -C firmware sim
python tools/simulator/simulate.py                                    # every scenario, nuisance matrix included
python tools/validation/replay.py --demo --out reports/replay-demo    # synthetic captures, replayed, with sweeps
npm run escalation:dry-run -- --incidents 20 --silent police --out reports/escalation-dry-run
python tools/validation/report.py reports/replay-demo.json reports/escalation-dry-run.json \
       --out reports/m4-rehearsal.md
```

Read `reports/m4-rehearsal.md` end to end. Check that you can say what every table would
mean with real data before spending a bench day.

## 3. The bench image

```bash
cd firmware && idf.py menuconfig      # FireX node -> Bench (M4) -> enable injection
idf.py build flash monitor
```

Expect the banner's firmware version to end in `+inject`, and `status` to say this is a
bench image. On the console:

```
inject flame 8        # the IR LED flickers at the flame sensor for 8 s
```

Within about 5 s: CRITICAL, the buzzer, a `media_captured` entry, and on the gateway an
incident. `inject` is refused outside NORMAL and during a self-test.

**The bench image never goes on a unit that can see people.** An injected CRITICAL fires the
camera, which makes the console a shutter. Flash the normal image before the unit leaves
the bench, and check that `status` no longer mentions it.

## 4. Session A: nuisance matrix and exposure trials (normal image)

Follow [validation.md](../validation.md) §2 to §4, with the normal image and the service
settings. Capture at 500 ms and mark every trial. Plan about four bench days for the matrix
and two for the exposures. Keep the record sheet as you go.

At the end of each day:

```bash
python tools/validation/replay.py captures/trials-<day>-*.csv --out reports/replay-<day>
```

Check the fidelity line before anything else. If the replay does not reproduce the node's
own state changes, stop and find out why (validation.md §5).

## 5. Tuning

Run the sweeps in validation.md §5 over the first half of the sessions, choose
`fx_mos_pair`, `fx_flame_alone` and any dwell change, and confirm them on the second half.
Enter each chosen value with `config set`, repeat three trials of every affected source,
and record it in the calibration record sheet. If the data cannot decide `fx_mos_pair` or
`fx_flame_alone`, say so in the report and leave the default for the pilot sites to decide.

## 6. Session B: 72-hour burn-in (bench image)

Follow validation.md §7. Start it on a Friday afternoon. Switch the Wi-Fi access point off
for 10 minutes twice during the run, and note each outage on the sheet.

`burnin.py` holds the serial port for all 72 hours. Close the IDF monitor and any capture
window, and stop the laptop from sleeping. A CRITICAL that no injection explains fails the
run, so keep the room free of cooking, incense and smoke. An injection refused because the
daily self-test was running is retried a minute later and is not counted as a miss; any
other refusal is.

```bash
python tools/validation/burnin.py run --port COMx --out captures/burnin-<date> --hours 72 \
       --inject-every-min 30 --inject-seconds 8 --status-every-min 10 --export-at-end
python tools/validation/burnin.py report captures/burnin-<date> --bundle captures/burnin-<date>-ledger.json \
       --json reports/burnin.json --md reports/burnin.md
npm run verify -- captures/burnin-<date>-ledger.json
```

The report must say PASS, and the exported ledger must verify.

## 7. Session C: camera (bench image)

Follow validation.md §8: reference frames from an injection in the lit room, ten smoulder
trials with the target in view, and the unlit room with an injection and a candle.

## 8. Session D: the alert path (bench image, gateway on the laptop)

Follow validation.md §9: mock endpoints with one silent service, the gateway with the HTTP
provider, a capture, and at least 20 CRITICALs.

```bash
python tools/validation/alert_latency.py --capture captures/alert-path-<date>.events.jsonl \
       --csv captures/alert-path-<date>.csv --endpoints captures/endpoints-<date>.jsonl \
       --out reports/alert-latency-bench
```

Every CRITICAL must have an alert delivered to every service in the registry. The silent
service must be chased no earlier than the acknowledgement timeout.

## 9. The report

```bash
python tools/validation/report.py reports/replay*.json reports/burnin.json reports/alert-latency-bench.json \
       reports/escalation-dry-run.json reports/camera-*.json --out reports/m4-reliability.md
```

When it carries no SYNTHETIC banner, commit it with its input JSON under `docs/reports/m4/`
and archive the captures and bundles it names.

## 10. M4 exit checklist

- [ ] At least 10 trials per nuisance source in the matrix, captured at 500 ms, each marked.
- [ ] At least 10 trials per hazard source. Miss rate and time-to-alarm are reported with
      95 % intervals.
- [ ] Replay fidelity: every node state change in the trial captures is matched by the
      replay.
- [ ] `fx_mos_pair` and `fx_flame_alone` are chosen from the data, or explicitly left to the
      pilot sites. Any change is entered on the node, visible as a `config_change` entry, and
      confirmed on captures not used to choose it.
- [ ] The 72-hour burn-in report passes: no fault entries, no watchdog, panic or brownout
      resets, every injection CRITICAL within 15 s, heap stable. Its exported ledger
      verifies.
- [ ] Camera: reference, smoke and unlit frames are checked. The report says whether the first
      frame shows the source and what an unlit room gives.
- [ ] The escalation dry run passes on the laptop.
- [ ] A bench alert-path latency distribution from at least 20 CRITICALs, with capture,
      gateway and mock endpoints on one clock.
- [ ] No re-alert fires before the configured timeout.
- [ ] The reliability report has no SYNTHETIC banner, and its inputs are archived with
      their hashes.

## 11. Not verified yet

- Every tool in this guide has run only on synthetic captures, logs and frames.
- `inject flame` has not run on a board. The IR LED 2 cm from the KY-026 is the M2
  self-test loopback, which is itself unverified. If the flame score does not pass 0.9
  under the LED, the injection cannot raise CRITICAL on flame alone.
- The clock mapping for the alert path assumes USB serial delay is small and never
  negative. It has not been measured.
- Replay starts from scores, so driver settings cannot be tuned from captures (validation.md
  §5).
- The escalation service has only talked to the mock endpoints. An escalation in progress is
  lost if the gateway restarts.
- The burn-in injects only the flame path. Gas and heat are covered by the trials, not by
  three days of repetition.
- Mosquito coils, frying and aerosols have only been guessed at in the simulator scenarios.
  Their shapes there are made up.
