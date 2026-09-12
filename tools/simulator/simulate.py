#!/usr/bin/env python3
"""
FireX scenario simulator.

Drives the real fusion engine (the same C that runs on the node) through
synthetic five-channel traces, so the rules can be exercised before any sensor
exists. Scenarios are YAML; the engine runs as a subprocess behind a line
protocol (firmware/sim/fusion_cli.c), which keeps a 32-bit toolchain and a
64-bit Python out of each other's way.

    make -C firmware sim
    python tools/simulator/simulate.py                      # every scenario
    python tools/simulator/simulate.py gas-leak-closed-room # just one
    python tools/simulator/simulate.py --csv out.csv        # write the trace

Exit code is non-zero if a scenario does not reach the states it declares,
which is what makes this usable in CI.
"""
from __future__ import annotations

import argparse
import csv
import random
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - a clearer message than a stack trace
    sys.exit("PyYAML is required: pip install -r tools/simulator/requirements.txt")

ROOT = Path(__file__).resolve().parents[2]
SCENARIOS = Path(__file__).resolve().parent / "scenarios"
MODALITIES = ["gas", "smoke", "flame", "heat", "humidity"]
STATES = ["WARMUP", "NORMAL", "ELEVATED", "CRITICAL"]


# --- the C ABI --------------------------------------------------------------

DEFAULTS = dict(
    enter=0.60, exit=0.40, elevated_ms=5000, critical_ms=10000,
    flame_alone=1, flame_alone_score=0.90, flame_alone_ms=3000,
    warmup_ms=180000, rh_context=0.75, heat_quiet=0.30, gas_suppress=0.50,
)


class Engine:
    """The compiled fusion engine, spoken to over a pipe."""

    def __init__(self, warmup_s: int):
        exe = ROOT / "firmware" / "build" / ("fusion_cli.exe" if sys.platform == "win32" else "fusion_cli")
        if not exe.exists():
            sys.exit("fusion engine not built: run `make -C firmware sim` first")
        self.proc = subprocess.Popen(
            [str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        cfg = dict(DEFAULTS, warmup_ms=warmup_s * 1000)
        self._send(
            "config {enter} {exit} {elevated_ms} {critical_ms} {flame_alone} "
            "{flame_alone_score} {flame_alone_ms} {warmup_ms} {rh_context} "
            "{heat_quiet} {gas_suppress}".format(**cfg)
        )

    def _send(self, line: str) -> str:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        return self.proc.stdout.readline().strip()

    def step(self, t_ms: int, scores: list[float], valid: list[bool]) -> dict:
        parts = " ".join(f"{s:.4f} {1 if v else 0}" for s, v in zip(scores, valid))
        fields = self._send(f"step {t_ms} {parts}").split()
        return {
            "state": int(fields[0]),
            "changed": fields[1] == "1",
            "fault": fields[2] == "1",
            "voters": int(fields[3]),
            "voter_count": int(fields[4]),
            "reason_a": int(fields[5]),
            "reason_b": int(fields[6]),
            "effective": [float(x) for x in fields[7:12]],
        }

    def close(self) -> None:
        assert self.proc.stdin
        self.proc.stdin.write("quit\n")
        self.proc.stdin.flush()
        self.proc.wait(timeout=5)


# --- scenarios --------------------------------------------------------------

@dataclass
class Scenario:
    name: str
    description: str
    duration_s: int
    interval_s: float
    channels: dict
    expect: list[str]
    note: str | None
    warmup_s: int
    invalid: list[str]

    @staticmethod
    def load(path: Path) -> "Scenario":
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        return Scenario(
            name=doc.get("name", path.stem),
            description=doc.get("description", ""),
            duration_s=int(doc.get("duration_s", 120)),
            interval_s=float(doc.get("interval_s", 2.0)),  # the DHT22 floor
            channels=doc.get("channels", {}),
            expect=list(doc.get("expect", [])),
            note=doc.get("note"),
            warmup_s=int(doc.get("warmup_s", 0)),
            invalid=list(doc.get("invalid", [])),
        )

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


def run(scenario: Scenario, rows: list[dict] | None = None) -> tuple[list[str], bool]:
    engine = Engine(scenario.warmup_s)
    seen: list[str] = []
    faulted = False
    t = 0.0

    try:
        while t <= scenario.duration_s:
            scores = [scenario.score(m, t) for m in MODALITIES]
            valid = [m not in scenario.invalid for m in MODALITIES]
            result = engine.step(int(t * 1000), scores, valid)
            state = STATES[result["state"]]
            faulted = faulted or result["fault"]

            if not seen or seen[-1] != state:
                seen.append(state)
                reason = ""
                if result["reason_a"] < 5:
                    names = [MODALITIES[result["reason_a"]]]
                    if result["reason_b"] < 5:
                        names.append(MODALITIES[result["reason_b"]])
                    reason = f"  ({' + '.join(names)})"
                print(f"    t={t:6.0f}s  ->  {state}{reason}")

            if rows is not None:
                row = {"scenario": scenario.name, "t": t, "state": state}
                row.update({m: round(scores[i], 3) for i, m in enumerate(MODALITIES)})
                row.update({f"eff_{m}": round(result["effective"][i], 3) for i, m in enumerate(MODALITIES)})
                rows.append(row)

            t += scenario.interval_s
    finally:
        engine.close()

    return seen, faulted


def main() -> int:
    parser = argparse.ArgumentParser(description="Drive the FireX fusion engine through synthetic traces.")
    parser.add_argument("scenario", nargs="?", help="run one scenario by file stem")
    parser.add_argument("--csv", help="write every sample to this CSV file")
    parser.add_argument("--seed", type=int, default=7, help="noise seed, for reproducible runs")
    args = parser.parse_args()

    random.seed(args.seed)
    files = sorted(SCENARIOS.glob("*.yaml"))
    if args.scenario:
        files = [f for f in files if f.stem == args.scenario]
        if not files:
            return print(f"no scenario named {args.scenario}") or 2

    rows: list[dict] | None = [] if args.csv else None
    failures = 0

    for path in files:
        scenario = Scenario.load(path)
        print(f"\n  {scenario.name}")
        print(f"  {scenario.description}")
        seen, faulted = run(scenario, rows)

        reached = seen[-1] if seen else "-"
        expected = scenario.expect
        ok = all(state in seen for state in expected) if expected else True
        if expected and "CRITICAL" not in expected and "CRITICAL" in seen:
            ok = False  # a nuisance scenario that alarmed is a failure
        failures += 0 if ok else 1

        print(f"    states: {' -> '.join(seen)}{'  (fault raised)' if faulted else ''}")
        print(f"    {'PASS' if ok else 'FAIL'}  expected {' -> '.join(expected) or 'nothing in particular'}, ended at {reached}")
        if scenario.note:
            print(f"    note: {scenario.note}")

    if rows is not None and args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\n  wrote {len(rows)} samples to {args.csv}")

    print(f"\n  {len(files) - failures}/{len(files)} scenarios behaved as declared")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
