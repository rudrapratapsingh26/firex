# Validation procedure (M4)

Status: M4, written before any trial, burn-in or dry run has been done on hardware. The
tools it names have run only on synthetic data. Revise it after the first bench session,
and record every revision in the table at the end.

M4 turns "the rules look right in the simulator" into measured numbers: how often the unit
misses a hazard, how often it alarms on something harmless, how long it takes, whether it
runs for three days without a fault, what the camera actually sees, and how long an alert
takes to leave the building. The milestone deliverable is one report built from those
measurements, `reports/m4-reliability.md` (section 10).

## 1. What is measured, and with what

| Roadmap item | Procedure | Tool | Output |
|--------------|-----------|------|--------|
| Replay harness: recorded traces in, transitions out, error rates in a report | §2, §5 | `tools/validation/replay.py` | `replay` JSON |
| Nuisance matrix | §3 | `tools/capture/capture.py`, then replay | trials in the `replay` JSON |
| Real gas, smoke and flame; rules tuned; false-positive rate, miss rate, time-to-alarm | §4, §5 | capture, then replay with sweeps | `replay` JSON |
| 72-hour burn-in with scheduled injections | §7 | `tools/validation/burnin.py` | `burnin` JSON |
| Camera-in-the-loop checks | §8 | `tools/camera/frame_check.py` | `camera` JSON |
| Escalation dry runs; alert-path latency | §9 | `npm run escalation:dry-run`, `npm run mock:endpoints`, `tools/validation/alert_latency.py` | `alert_latency` JSON |
| Reliability report | §10 | `tools/validation/report.py` | Markdown |

Every JSON output says whether it came from synthetic data. The report generator prints a
SYNTHETIC banner if any input did, and never presents such a number as a measurement.

## 2. Recording a trial session

**Before.** The node is calibrated (`docs/calibration.md`), warmed up, in NORMAL, with the
fan at its service duty. Save `config list` to `captures/<session>-config.txt`: replay needs
the settings the node ran on.

**Capture at the fusion period.** Fusion runs every 500 ms, so record at 500 ms. At the
default 1000 ms the replay sees half of what fusion saw and its transitions drift from the
node's.

```bash
python tools/capture/capture.py --port COMx --out captures/trials-2026-10-06-a --period 500
```

**Mark every trial.** In the capture window, type the mark as the exposure starts and again
when it is removed. The node timestamps a mark when it arrives, on the same clock as the
samples.

```
trial nuisance cooking-toast-2m        exposure starts
trial end                              exposure removed
trial hazard flame-lighter-50cm
trial end
```

The class is `hazard` or `nuisance`. The category is the name up to its first hyphen:
`cooking`, `steam`, `incense`, `aerosol`, `dust` or `sunlight` for nuisances; `gas`,
`smoke`, `flame`, `heat` or `smoulder` for hazards; anything else counts as `other`. The rest
of the name is free text: source, distance, anything that tells the trials apart.

**A hazard trial is an exposure that must end in CRITICAL**: two sensing physics agreeing, or
an open flame. A gas release on its own is designed to stop at ELEVATED. Record such
single-signal exposures with plain marks for calibration, not as hazard trials, or the
report will count the design as a miss.

**Keep trials independent.** Between trials, wait until the node has been NORMAL for two
minutes and every score is below `fx_exit`. Ventilate between aerosol, smoke and incense
trials. If a trial starts early, note it on the sheet.

**A missed mark** is fixed afterwards with a labels file instead of editing the capture:

```csv
capture,class,name,start_s,end_s
trials-2026-10-06-a,nuisance,steam-kettle-1m,1832.5,1952.0
```

`start_s` and `end_s` are device seconds, as in the capture's `t_ms` column divided by 1000.
A labels file replaces every mark of the captures it names, so list all of that capture's
trials in it, not only the missed one. A trial left open is closed at the next trial or at
the end of the capture, with a warning.

**Record sheet, one row per trial:** session, capture file, trial name, source and dose,
distance, fan duty, room and ventilation, operator, second person, anything unusual.

## 3. The nuisance matrix

Each row is a source the unit will meet in a hostel, a ward or a canteen. The expected
response is the one the calibration checks already demand (`docs/calibration.md` §9).

| Category | Source | How | Expected | Fails if |
|----------|--------|-----|----------|----------|
| cooking | Toast, lightly browned | toaster 2 m from the unit | at most ELEVATED | CRITICAL |
| cooking | Frying | onions in oil on a hot plate, 2 m, 5 min | at most ELEVATED | CRITICAL |
| steam | Kettle | boiling at 1 m for 2 min | NORMAL, or ELEVATED on gas | CRITICAL |
| steam | Shower | hot shower, door closed, unit in the bathroom, 10 min | NORMAL, or ELEVATED on gas | CRITICAL |
| incense | Incense stick | one stick burning at 1 m for 15 min | at most ELEVATED | CRITICAL |
| incense | Mosquito coil | burning at 1 m for 15 min | at most ELEVATED | CRITICAL |
| aerosol | Deodorant | 3 s spray toward the unit from 1 m | at most ELEVATED | CRITICAL |
| aerosol | Insect spray or room freshener | 3 s spray into the room, 2 m | at most ELEVATED | CRITICAL |
| dust | Sweeping | brooming the floor under the unit, 2 min | NORMAL | ELEVATED or CRITICAL |
| sunlight | Direct sun | sunlight through a window onto the flame sensor, 2 min | NORMAL | flame score above `fx_enter` |
| sunlight | Moving shadows | a hand, blinds or a ceiling fan between a lamp and the sensor, 1 min | NORMAL | flame score above `fx_enter` |
| sunlight | Lamp and torch | a halogen or incandescent lamp switched on at 1 m; a phone torch waved for 30 s | NORMAL | flame score above `fx_enter` |

**Aerosols are the case to watch.** MQ-2 and MQ-135 are both heated metal-oxide sensors. A
propellant such as butane drives both, and with `fx_mos_pair` at its default of 1, gas and
smoke agreeing for 10 s is CRITICAL. The software roadmap says CRITICAL needs two
independent sensing physics. These two are not independent. §5 explains how the data
decides the setting.

**How many.** At least 10 trials per source. The table shows what a clean result proves:
the upper end of the 95 % interval when nothing happened in `n` trials.

| Trials with no event | Rate is below, with 95 % confidence |
|----------------------|-------------------------------------|
| 5 | 43 % |
| 10 | 28 % |
| 20 | 16 % |
| 30 | 11 % |
| 60 | 6 % |

Ten trials cannot show that a nuisance rate is low, only that it is not high. Say that in
the report rather than printing 0 %.

**Time.** Twelve sources at ten trials each, with recovery between trials, is roughly four
bench days. Do aerosols, incense and cooking last in a session, because their residue
lingers.

## 4. Exposure trials

Safety, for every trial in this section: small doses, a ventilated room, nothing else lit,
a fire extinguisher or blanket within reach, a second person present, and never a gas
release and an open flame at the same time near the MQ heaters.

| Category | Source | How | Expected |
|----------|--------|-----|----------|
| flame | Lighter at 50 cm | held facing the flame sensor for 10 s | CRITICAL on flame alone |
| flame | Lighter at 1 m | as above | CRITICAL on flame alone |
| flame | Candle at 1 m | lit, left burning 60 s | CRITICAL on flame alone |
| smoulder | Smouldering cotton or paper | in a metal tin at 1 m, a hair dryer on low 50 cm from the DHT22 after 60 s | CRITICAL on smoke + heat, or gas + smoke with `fx_mos_pair` 1 |
| gas | Reference gas dose with heat | the calibration reference dose at 30 cm and a hair dryer on the DHT22 | CRITICAL on gas + heat within 60 s |
| heat | Hair dryer with smoke | the hair dryer on the DHT22 while a smouldering source is at 1 m | CRITICAL on heat + smoke |

At least 10 trials per source. Type `trial hazard ...` at the moment the exposure starts:
time-to-alarm is measured from that mark.

Time-to-alarm includes the rule dwell by design: 3 s for flame alone and 10 s for two
agreeing signals, plus however long the source takes to push a score past `fx_enter`.

## 5. Replay and tuning

Replay feeds the scores the node recorded into the same compiled fusion engine that runs on
the node and in CI. It gives back the transitions and scores every trial.

```bash
make -C firmware sim
python tools/validation/replay.py captures/trials-*.csv --labels captures/trials-labels.csv --out reports/replay
```

`--config` takes node setting keys in node units, so any value found here is a
`config set` command on the node. Give the values from the session's `config list` if
they differ from the defaults. The engine runs on the node's own uptime clock, so warm-up
is replayed as the node lived it: a capture started after warm-up is warm, and a reboot in
the middle of a capture blinds the replay for `fx_warmup_s` just as it blinded the node.

**Fidelity first.** The report compares the node's own state changes in each capture with
the replay's. If they do not match, the replay is not the node, and its error rates mean
nothing until the cause is found. The usual causes are a capture period above 500 ms, a
`config set` in the middle of the capture (reported), or replay settings that differ from
the node's.

**Sweeps** replay the same trials under several rule sets and put the results side by side:

```bash
python tools/validation/replay.py captures/trials-*.csv --sweep fx_mos_pair=1,0 --sweep fx_flame_alone=1,0 \
       --sweep fx_crit_ms=6000,10000,15000 --out reports/replay-sweep
```

Two decisions are waiting for this data:

- **`fx_mos_pair`.** With 1, gas + smoke can raise CRITICAL. Aerosols and some cooking may
  false-alarm, but a smouldering fire that produces smoke and gas before any heat is caught
  early. With 0, those aerosol trials stay ELEVATED, and smouldering waits for heat. Compare
  the aerosol and cooking false-alarm rates with smoulder time-to-alarm and misses.
- **`fx_flame_alone`.** With 1, an open flame is CRITICAL in about 3 s; sunlight, shadows
  and a welding arc are the risk. Compare the sunlight false-alarm rate with flame
  time-to-alarm under 0, where a flame waits for heat or smoke.

**Rules for adopting a tuned value.**

1. Choose on one half of the captures and confirm on the other half, or on a new session.
   A value chosen and scored on the same trials looks better than it is.
2. Enter it on the node with `config set`, so it becomes a `config_change` entry, then
   repeat at least three trials of every source it could affect.
3. Record it in the calibration record sheet with the report that justified it.

**What replay cannot tune.** Driver settings (`mq2_full`, `mq135_full`, `flame_quiet`,
`flame_full`, the heat curve) act before the score is computed, and replay starts from
scores. Changing them needs new captures.

## 6. Metric definitions

These are exactly what `tools/validation/fxreplay.py` computes.

- **Trial window**: from the start mark to the end mark plus a grace period (default 60 s),
  because a fire does not stop when the source is removed.
- **Detected**: a hazard trial whose replayed state reaches CRITICAL inside its window.
  **Missed**: one that does not. **Miss rate** = missed / hazard trials.
- **Time-to-alarm**: the first CRITICAL in the window minus the start mark, per detected
  trial; reported as median, 90th percentile and maximum.
- **False alarm**: a nuisance trial whose replayed state reaches CRITICAL inside its window.
  **False-alarm rate** = false alarms / nuisance trials, overall and per category.
- **Elevated**: a nuisance trial that reached ELEVATED but not CRITICAL. This is not a
  failure, but a source that is usually ELEVATED will be heard about from the site.
- **Background false alarms**: transitions into CRITICAL outside every trial window,
  reported per 24 hours of background time.
- **Rates** carry the 95 % Wilson score interval.
- **Fidelity**: node state changes matched by a replayed change to the same state within
  twice the sample period plus 1 s, out of all node state changes in the captures.

## 7. The 72-hour burn-in

The burn-in shows the unit runs for three days, alarming on schedule, without a fault or a
reset. Nobody has to stand at the bench: injections come from the node's own self-test IR
LED, which makes the real flame sensor see flicker.

**Set-up.**

1. Build and flash the bench image: `idf.py menuconfig` → FireX node → Bench (M4) →
   injection. Its firmware version ends in `+inject`, in the banner, the `boot` entry and
   on the gateway. Every entry it writes carries that image's `fw_hash`.
2. The IR LED from the M2 wiring faces the KY-026 (`FIREX_FLAME_TEST_LED_GPIO`); the camera
   is connected; the unit is provisioned to a gateway so photos are acknowledged, the ledger
   is mirrored and rotation does not lose entries.
3. The gateway's alert provider is `off`, or `http` pointed at mock endpoints. Never
   anything that reaches a person: the burn-in raises about 144 CRITICALs.
4. Put the buzzer in a sound-deadening box. Do not disconnect it: the burn-in is also a
   test of the buzzer path.
5. For more self-tests than the daily default, `config set selftest_h 4` (18 over the run);
   restore it afterwards. Both changes are logged. An injection that arrives while a
   self-test holds the IR LED is refused; the runner retries it a minute later, and the
   report counts the retry, not the refusal. Any other refusal is a miss.

**Run.**

```bash
python tools/validation/burnin.py run --port COMx --out captures/burnin-2026-10-13 \
       --hours 72 --inject-every-min 30 --inject-seconds 8 --status-every-min 10 --export-at-end
python tools/validation/burnin.py report captures/burnin-2026-10-13 \
       --bundle captures/burnin-2026-10-13-ledger.json --json reports/burnin.json --md reports/burnin.md
```

The runner records the stream as `capture.py` does. It sends each injection and polls
`status` for free heap, and it reopens the port if the node reboots or the cable drops,
logging the gap. At the end it exports the ledger, which is the authoritative record of
faults, boots and rotations.

**Pass criteria**, as the report checks them:

| Criterion | Pass |
|-----------|------|
| Fault entries | none raised |
| Resets | no `watchdog`, `panic` or `brownout` boot; any other boot is listed |
| Injections | every injection followed by CRITICAL within 15 s; a refused injection counts as a miss, with its reason |
| Self-tests | every `self_test` entry passes |
| Ledger | no ledger write error; no `log_overwrite` with `mirrored: false` |
| Memory | free heap never below 30 000 bytes and not falling faster than 2 000 bytes an hour |

**What the burn-in does not cover.** Only the flame path is injected: gas and heat are
exercised by §3 and §4, not by the burn-in. Wi-Fi outages have to be caused by hand: switch
the access point off for 10 minutes twice during the run and mark each outage. The time from
CRITICAL to the buzzer GPIO needs a logic analyser, as in the M2 guide.

## 8. Camera-in-the-loop

Two questions from the roadmap: does the first frame still show the seat of the fire once
smoke builds, and what does the camera give in an unlit room.

1. **Reference.** Lit room, clear air, a visible target where the source will be (the tin, a
   marked card). On the bench image, `inject flame 8`. The CRITICAL captures frames as it
   would in an incident. These are the reference frames. No remote shutter is involved.
2. **Smoke.** Run the smoulder trial from §4 with the target in view, ten times. Keep each
   trial's frames.
3. **Unlit.** Lights off, curtains closed. Repeat the reference with `inject flame`, then
   with a candle as the source (§4 safety).
4. Export the frames (`tools/ledger/export_log.py --media`, or from the gateway) and run:

   ```bash
   pip install -r tools/camera/requirements.txt
   python tools/camera/frame_check.py --reference captures/cam-ref --frames captures/cam-smoke-01 --out reports/camera-smoke-01
   ```

   Each frame gets its mean brightness, contrast, sharpness and similarity to the reference,
   with the flags `dark`, `obscured` and `blurred`.
5. Two people look at every frame independently and answer one question on the sheet: is the
   source visible? The metrics support that judgement; they do not replace it.

The time from CRITICAL to the first stored frame is the gap between the `state_change` and
`media_captured` events in the capture.

If unlit frames are black, the answer may be the ESP32-CAM's flash LED on low light. That is
a firmware change and a media-policy change for a later milestone, not something to switch
on during M4.

## 9. Escalation dry runs and the alert path

No alert goes to a real service in M4. The gateway's HTTP alert provider points at mock
endpoints that record when each alert arrives and acknowledge on a schedule, or never.

**On a laptop, no hardware.** Simulated units, the real gateway ingest and escalation
service, and mock endpoints, all in one process:

```bash
npm run escalation:dry-run -- --incidents 20 --silent police --out reports/escalation-dry-run
```

This measures the gateway's own code and the re-alert timer. It checks that every alert is
delivered, that acknowledgements are recorded, and that the silent service is chased no
earlier than its timeout. The units and the services are stand-ins, so crossing to CRITICAL
is only the dwell the dry run was given; the report says so beside the numbers. The gateway
segments are real timings of real code, which is why the dry run's output is not marked
synthetic.

**On the bench.** Capture, gateway and mock endpoints on one laptop, so they share a clock:

```bash
npm run mock:endpoints -- --port 9090 --log captures/endpoints-2026-10-20.jsonl --ack police=silent
FIREX_SERVICE_REGISTRY=gateway/escalation/registry.example.json FIREX_ALERT_PROVIDER=http \
FIREX_ALERT_HTTP_URL=http://127.0.0.1:9090 npm run gateway
python tools/capture/capture.py --port COMx --out captures/alert-path-2026-10-20 --period 500
```

Drive at least 20 CRITICALs: `inject flame 8` on the bench image, or the hazard trials of
§4. Then:

```bash
python tools/validation/alert_latency.py --capture captures/alert-path-2026-10-20.events.jsonl \
       --csv captures/alert-path-2026-10-20.csv --endpoints captures/endpoints-2026-10-20.jsonl \
       --out reports/alert-latency-bench
```

The path is reported in segments, each a distribution:

| Segment | From | To |
|---------|------|----|
| crossing to CRITICAL | the earliest threshold crossing among the signals that agreed | the CRITICAL decision on the node |
| CRITICAL to gateway | the CRITICAL decision | the gateway storing the verified entry |
| gateway to delivered | the gateway storing it | the mock endpoint receiving the alert |
| crossing to delivered | the threshold crossing | the alert received |

Node times are device milliseconds, placed on the laptop clock by the smallest gap seen
between a serial line's device time and its arrival. Serial delay only adds, so the
smallest gap is the best estimate. The first segment is mostly the rule dwell, by design.

## 10. The reliability report

```bash
python tools/validation/report.py reports/replay.json reports/burnin.json reports/alert-latency-bench.json \
       reports/escalation-dry-run.json reports/camera-*.json --out reports/m4-reliability.md
```

The report lists every input with its SHA-256. It states error rates with their intervals
in words, compares rule sets when a sweep was run, shows the burn-in criteria, the latency
distributions (bench and dry run kept apart), and the camera results. It ends with the M4
exit criteria marked met, not met, or no data.

`reports/` is not in git. When the report has no SYNTHETIC banner, commit it with its input
JSON under `docs/reports/m4/`, and archive the captures and exported bundles it names
somewhere that keeps them. They are the evidence behind every number in it.

## Revisions

| Date | Change | Why |
|------|--------|-----|
| 2026-09-15 | First version | M4 |
