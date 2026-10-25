#!/usr/bin/env python3
"""
FireX scenario simulator.

Drives the real fusion engine (the same C that runs on the node) through
synthetic five-channel traces, so the rules can be exercised before any sensor
exists. Scenarios are YAML; the engine runs as a subprocess behind a line
protocol (firmware/sim/fusion_cli.c, spoken to by fxengine.py).

    make -C firmware sim
    python tools/simulator/simulate.py                      # every scenario
    python tools/simulator/simulate.py gas-leak-closed-room # just one
    python tools/simulator/simulate.py --csv out.csv        # write the trace
    python tools/simulator/simulate.py --capture captures/synthetic --repeat 5
                                                            # SYNTHETIC captures for the replay harness

Exit code is non-zero if a scenario does not reach the states it declares,
which is what makes this usable in CI. With --capture the files are the point
and the exit code only says whether they were written.

A scenario may carry `config:` (node setting keys, e.g. `fx_mos_pair: 0`) and a
`trial:` block ({class, name, start_s, end_s}) that --capture turns into the
`trial <class> <name>` / `trial end` marks an operator types on the bench.
"""
from __future__ import annotations

import argparse
import csv
import json
import random
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxengine import MODALITIES, STATES, Engine, EngineNotBuilt, reason_names  # noqa: E402

try:
    import yaml
except ImportError:  # pragma: no cover - a clearer message than a stack trace
    sys.exit("PyYAML is required: pip install -r tools/simulator/requirements.txt")

SCENARIOS = Path(__file__).resolve().parent / "scenarios"

# Synthetic captures start ten minutes after a notional boot, with the host
# clock at a fixed instant, so every regenerated file is byte-identical.
CAPTURE_T0_MS = 600_000
CAPTURE_EPOCH_MS = 1_789_000_000_000
CAPTURE_SEQ0 = 100
CAPTURE_COLUMNS = ["t_ms", "gas", "gas_ok", "smoke", "smoke_ok", "flame", "flame_ok", "heat", "heat_ok",
                   "rh", "rh_ok", "fan_duty", "fan_rpm", "fan_state", "alarm", "host_s", "host_unix_ms"]


# --- scenarios --------------------------------------------------------------

@dataclass
class Scenario:
    name: str
    stem: str
    description: str
    duration_s: int
    interval_s: float
    channels: dict
    expect: list[str]
    note: str | None
    warmup_s: int
    invalid: list[str]
    config: dict = field(default_factory=dict)
    trial: dict | None = None
    fan: dict = field(default_factory=dict)

    @staticmethod
    def load(path: Path) -> "Scenario":
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        trial = doc.get("trial")
        if trial is not None:
            missing = [k for k in ("class", "name", "start_s", "end_s") if k not in trial]
            if missing or trial["class"] not in ("hazard", "nuisance"):
                raise ValueError(f"{path.name}: trial needs class hazard|nuisance, name, start_s, end_s")
        return Scenario(
            name=doc.get("name", path.stem),
            stem=path.stem,
            description=doc.get("description", ""),
            duration_s=int(doc.get("duration_s", 120)),
            interval_s=float(doc.get("interval_s", 2.0)),  # the DHT22 floor
            channels=doc.get("channels", {}),
            expect=list(doc.get("expect", [])),
            note=doc.get("note"),
            warmup_s=int(doc.get("warmup_s", 0)),
            invalid=list(doc.get("invalid", [])),
            config=dict(doc.get("config") or {}),
            trial=trial,
            fan=dict(doc.get("fan") or {}),
        )

    @property
    def settings(self) -> dict:
        """What the engine runs with: the scenario's warm-up, then its own overrides."""
        return {"fx_warmup_s": self.warmup_s, **self.config}

    def score(self, modality: str, t: float) -> float:
        """Piecewise-linear segments with optional noise, evaluated at time t."""
        spec = self.channels.get(modality)
        if spec is None:
            return 0.0
        if isinstance(spec, (int, float)):
            return float(spec)

        value, previous_until = 0.0, 0.0
        for segment in spec:
            until = float(segment.get("until", self.duration_s))
            start = float(segment.get("from", value))
            end = float(segment.get("to", start))
            if t <= until:
                span = max(until - previous_until, 1e-6)
                value = start + (end - start) * ((t - previous_until) / span)
                break
            value, previous_until = end, until
        else:
            value = float(spec[-1].get("to", value))

        noise = float(self.channels.get(f"{modality}_noise", 0.0))
        if noise:
            value += random.uniform(-noise, noise)
        return max(0.0, min(1.0, value))


@dataclass
class Step:
    t: float          # scenario seconds
    scores: list[float]
    valid: list[bool]
    result: dict

    @property
    def state(self) -> str:
        return STATES[self.result["state"]]


def trace(scenario: Scenario, t0_ms: int = 0) -> list[Step]:
    """Every sample of the scenario through the engine, fed at t0_ms + t."""
    steps: list[Step] = []
    engine = Engine(scenario.settings)
    try:
        t = 0.0
        while t <= scenario.duration_s:
            scores = [scenario.score(m, t) for m in MODALITIES]
            valid = [m not in scenario.invalid for m in MODALITIES]
            steps.append(Step(t, scores, valid, engine.step(t0_ms + int(round(t * 1000)), scores, valid)))
            t += scenario.interval_s
    finally:
        engine.close()
    return steps


def run(scenario: Scenario, rows: list[dict] | None = None) -> tuple[list[str], bool]:
    seen: list[str] = []
    faulted = False
    for step in trace(scenario):
        state = step.state
        faulted = faulted or step.result["fault"]
        if not seen or seen[-1] != state:
            seen.append(state)
            names = reason_names(step.result)
            reason = f"  ({' + '.join(names)})" if names else ""
            print(f"    t={step.t:6.0f}s  ->  {state}{reason}")

        if rows is not None:
            row = {"scenario": scenario.name, "t": step.t, "state": state}
            row.update({m: round(step.scores[i], 3) for i, m in enumerate(MODALITIES)})
            row.update({f"eff_{m}": round(step.result["effective"][i], 3) for i, m in enumerate(MODALITIES)})
            rows.append(row)
    return seen, faulted


def judge(scenario: Scenario, seen: list[str]) -> bool:
    expected = scenario.expect
    ok = all(state in seen for state in expected) if expected else True
    if expected and "CRITICAL" not in expected and "CRITICAL" in seen:
        ok = False  # a nuisance scenario that alarmed is a failure
    return ok


# --- synthetic captures -------------------------------------------------------

def write_capture(scenario: Scenario, directory: Path, repeat: int = 1, seed: int = 7) -> tuple[Path, Path, list[str]]:
    """One SYNTHETIC capture in the format tools/capture/capture.py writes:
    <stem>-r<repeat>.csv with the node's score columns, and .events.jsonl with a
    `synthetic` mark, the trial marks and a state_change echo per transition."""
    random.seed(f"{seed}-{scenario.stem}-{repeat}")
    directory.mkdir(parents=True, exist_ok=True)
    base = directory / f"{scenario.stem}-r{repeat}"
    csv_path = base.with_name(base.name + ".csv")
    events_path = base.with_name(base.name + ".events.jsonl")

    steps = trace(scenario, CAPTURE_T0_MS)
    fan_duty = scenario.fan.get("duty", 100)
    fan_rpm = scenario.fan.get("rpm", 1450)
    fan_state = scenario.fan.get("state", "on")

    def clock(t: float) -> tuple[int, float, int]:
        rel_ms = int(round(t * 1000))
        return CAPTURE_T0_MS + rel_ms, round(t, 3), CAPTURE_EPOCH_MS + rel_ms

    events: list[tuple[float, dict]] = [(0.0, {"type": "mark", "text": f"synthetic {scenario.stem}"})]
    if scenario.trial:
        events.append((float(scenario.trial["start_s"]),
                       {"type": "mark", "text": f"trial {scenario.trial['class']} {scenario.trial['name']}"}))
        events.append((float(scenario.trial["end_s"]), {"type": "mark", "text": "trial end"}))

    seq = CAPTURE_SEQ0
    previous = "WARMUP"
    seen: list[str] = []
    with open(csv_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(CAPTURE_COLUMNS)
        for step in steps:
            t_ms, host_s, host_unix_ms = clock(step.t)
            cells: list[object] = [t_ms]
            for i in range(len(MODALITIES)):
                cells += [f"{step.scores[i]:.3f}", 1 if step.valid[i] else 0]
            writer.writerow(cells + [fan_duty, fan_rpm, fan_state, step.state, host_s, host_unix_ms])
            if step.state != previous:
                scores = {m: int(round(step.result["effective"][i] * 100))
                          for i, m in enumerate(MODALITIES) if step.valid[i]}
                events.append((step.t, {"type": "state_change", "seq": seq, "from": previous, "to": step.state,
                                        "reason": reason_names(step.result), "scores": scores}))
                seq += 1
                previous = step.state
            if not seen or seen[-1] != step.state:
                seen.append(step.state)

    events.sort(key=lambda item: item[0])
    with open(events_path, "w", encoding="utf-8") as handle:
        for t, body in events:
            t_ms, host_s, host_unix_ms = clock(t)
            handle.write(json.dumps({"t_ms": t_ms, "host_s": host_s, "host_unix_ms": host_unix_ms, **body}) + "\n")
    return csv_path, events_path, seen


def write_captures(files: list[Path], directory: Path, repeat: int = 1, seed: int = 7,
                   only_trials: bool = False) -> list[Path]:
    """SYNTHETIC captures for each scenario file, `repeat` noise draws each. Returns the CSV paths."""
    written: list[Path] = []
    for path in files:
        scenario = Scenario.load(path)
        if only_trials and not scenario.trial:
            continue
        for r in range(1, repeat + 1):
            csv_path, _, _ = write_capture(scenario, directory, r, seed)
            written.append(csv_path)
    return written


# --- command line -------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Drive the FireX fusion engine through synthetic traces.")
    parser.add_argument("scenario", nargs="?", help="run one scenario by file stem")
    parser.add_argument("--csv", help="write every sample to this CSV file")
    parser.add_argument("--seed", type=int, default=7, help="noise seed, for reproducible runs")
    parser.add_argument("--capture", help="write SYNTHETIC captures (CSV + events) into this directory")
    parser.add_argument("--repeat", type=int, default=1, help="with --capture: noise draws per scenario")
    args = parser.parse_args()

    random.seed(args.seed)
    files = sorted(SCENARIOS.glob("*.yaml"))
    if args.scenario:
        files = [f for f in files if f.stem == args.scenario]
        if not files:
            return print(f"no scenario named {args.scenario}") or 2

    try:
        if args.capture:
            return capture_main(files, Path(args.capture), max(1, args.repeat), args.seed)
        return scenarios_main(files, args.csv)
    except EngineNotBuilt as exc:
        sys.exit(str(exc))


def capture_main(files: list[Path], directory: Path, repeat: int, seed: int) -> int:
    for path in files:
        scenario = Scenario.load(path)
        for r in range(1, repeat + 1):
            csv_path, _, seen = write_capture(scenario, directory, r, seed)
            verdict = "as declared" if judge(scenario, seen) else "NOT as declared"
            print(f"  {csv_path.name}: {' -> '.join(seen)} ({verdict})")
    print(f"\n  SYNTHETIC captures in {directory}: made-up traces, not measurements")
    return 0


def scenarios_main(files: list[Path], csv_out: str | None) -> int:
    rows: list[dict] | None = [] if csv_out else None
    failures = 0

    for path in files:
        scenario = Scenario.load(path)
        print(f"\n  {scenario.name}")
        print(f"  {scenario.description}")
        seen, faulted = run(scenario, rows)

        reached = seen[-1] if seen else "-"
        ok = judge(scenario, seen)
        failures += 0 if ok else 1

        print(f"    states: {' -> '.join(seen)}{'  (fault raised)' if faulted else ''}")
        print(f"    {'PASS' if ok else 'FAIL'}  expected {' -> '.join(scenario.expect) or 'nothing in particular'}, ended at {reached}")
        if scenario.config:
            print(f"    config: {', '.join(f'{k}={v}' for k, v in scenario.config.items())}")
        if scenario.note:
            print(f"    note: {scenario.note}")

    if rows is not None and csv_out:
        with open(csv_out, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\n  wrote {len(rows)} samples to {csv_out}")

    print(f"\n  {len(files) - failures}/{len(files)} scenarios behaved as declared")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
