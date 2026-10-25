# ADR 0006 — How M4 measures the unit

Date: 2026-09-15
Status: accepted for M4

## Context

M4 has to produce numbers an evaluator can check: miss rate, false-alarm rate and
time-to-alarm for the fusion rules; three days without a fault; what the camera sees; and
how long an alert takes to leave the building. One developer runs the bench, trials are
counted in tens rather than thousands, there is no automated exposure rig, and the node's
ledger clock is still `uncertain`. The escalation service existed only as a library and a
demo.

## Decisions

**Replay the node's own scores through the compiled engine.** A trial session is a serial
capture at the 500 ms fusion period, with trials marked on the device clock (`trial hazard
...`, `trial end`). The replay harness feeds the recorded scores to the same `fusion_cli`
the simulator uses and scores each trial against its label. Rule settings can be swept
offline and compared side by side, named by the node's own setting keys so a chosen value is
a `config set`. Before any rate is trusted, the replay must reproduce the node's own state
changes.

**Intervals on every rate, and synthetic data never presented as a measurement.** Rates carry
their 95 % Wilson interval, because ten clean trials bound a rate only below 28 %. Every tool
writes JSON that says whether its input was synthetic, and the report generator puts a banner
on any report built from such input.

**A switch for the metal-oxide pair, default unchanged.** MQ-2 and MQ-135 are the same sensing
physics, and an aerosol propellant drives both, yet gas and smoke agreeing counts as two
modalities for CRITICAL. `fx_mos_pair` (default 1, today's behaviour) set to 0 requires flame
or heat among the agreeing signals. Changing the default without data would trade aerosol
false alarms for later detection of smouldering fires, blind. The nuisance matrix and the
smoulder trials decide it, or hand it to the pilot sites, as with `fx_flame_alone`.

**Scheduled injections through the real flame sensor.** The burn-in fires CRITICAL every 30
minutes by flickering the self-test IR LED at the KY-026 from the console (`inject flame`). The
sensor, scorer, fusion, alarm, camera, ledger and network all run as they would in a fire.
The command exists only in an image built with `CONFIG_FIREX_BENCH_INJECT`, whose firmware
version ends in `+inject`. That version is in the `boot` entry, and the image's `fw_hash` is
in every entry. Injections are echoed on serial, not written as a new ledger type, so the
ledger specification does not change.

Rejected: injecting scores in software, which skips the sensors and puts a code path that
fakes a CRITICAL into the firmware; and relay-driven gas or heat sources, which cost hardware
and a safety case that one developer cannot justify for a burn-in. The trials cover gas and
heat instead.

**Escalation runs on the gateway, from the verified CRITICAL entry.** A new incident whose
CRITICAL `state_change` entry verified (`ok`, `start` or `gap`) starts the escalation service.
The unsigned `state` message never does. Alert events are stored in the gateway database.
The ledger types `alert_sent`, `alert_ack` and `alert_timeout` stay reserved for M5, which
has to decide who signs them: the gateway holds no ledger key. In M4 the only providers are
the console and an HTTP provider pointed at mock endpoints, which record arrivals and
acknowledge on a schedule or never.

**Latency on one clock.** The alert path is measured in segments with the capture, gateway and
mock endpoints on one host. Device milliseconds are placed on that clock by the smallest gap
between a serial line's device time and its arrival. The escalation service compares times in
fractional seconds, so a re-alert can never fire before its window. Values it writes into
payloads are rounded to whole seconds, because the ledger refuses floats.

## Consequences

- Every M4 procedure has a tool, a JSON output and a place in one report, and the whole
  pipeline can be rehearsed on a laptop before the first trial.
- Driver settings (`mq2_full`, `flame_quiet`, the heat curve) cannot be tuned by replay, since
  it starts from scores. They need new captures.
- The bench image is a remote shutter while it is on the bench. It must never be fitted to a
  unit that can see people, and `status` says when it is running.
- The burn-in exercises the flame path for three days. Gas and heat are exercised only in the
  trials.
- Bench latency numbers depend on one laptop running capture, gateway and mock endpoints. On
  the Pi, the gateway segment needs a clock shared with the capture host, and a node clock
  synchronised from the gateway is still not built.
- An escalation in progress is lost if the gateway restarts. M5 must persist or resume it
  before any real service is connected.
- A CRITICAL entry that reaches the gateway late, after a unit's outage, opens its incident
  and alerts when it arrives, however old it is. The node's clock is `uncertain`, so the
  gateway cannot yet tell how old it is. M5 has to decide what a late alarm should do before
  a real service receives one.
