#!/usr/bin/env python3
"""
Replay recorded captures through the real fusion engine and measure error rates.

    make -C firmware sim
    python tools/validation/replay.py captures/m4-*.csv --out reports/replay
    python tools/validation/replay.py captures/m4-*.csv --labels captures/trials.csv
    python tools/validation/replay.py captures/m4-*.csv --sweep fx_mos_pair=1,0 --sweep fx_crit_ms=6000,10000
    python tools/validation/replay.py --demo --out reports/replay-demo   # SYNTHETIC, from the scenarios

Writes <out>.json (for report.py) and <out>.md. Each configuration is named by the
node's setting keys and units, so a chosen value goes straight into
`config set <key> <value>`. --config sets the base; every --sweep multiplies the
configurations by its values. Replay starts from the node's defaults, including
its 180 s warm-up counted from the node's uptime clock.

The trial grammar, the outcome definitions and what replay cannot tell you are in
fxreplay.py and docs/validation.md.
"""
from __future__ import annotations

import argparse
import glob
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fxreplay  # noqa: E402
import report  # noqa: E402
from fxengine import EngineNotBuilt, available, binary_path  # noqa: E402  (fxreplay put simulator/ on the path)
from fxstats import rate_text, summary_text  # noqa: E402

DEMO_SWEEPS = ["fx_mos_pair=1,0", "fx_flame_alone=1,0"]


def expand(patterns: list[str]) -> list[Path]:
    paths: list[Path] = []
    for pattern in patterns:
        matches = sorted(glob.glob(pattern)) if any(ch in pattern for ch in "*?[") else [pattern]
        paths += [Path(m) for m in matches]
    return paths


def demo_captures(directory: Path) -> list[Path]:
    sys.path.insert(0, str(HERE.parent / "simulator"))
    try:
        import simulate  # noqa: E402  (needs PyYAML for the scenario files)
    except SystemExit as exc:
        raise RuntimeError(str(exc)) from None
    files = sorted(simulate.SCENARIOS.glob("*.yaml"))
    return simulate.write_captures(files, directory, repeat=5, seed=7, only_trials=True)


def headline(doc: dict) -> None:
    tag = "  [SYNTHETIC]" if doc["synthetic"] else ""
    for cfg in doc["configs"]:
        print(f"\n  {cfg['name']}{tag}")
        print(f"    hazards detected    {rate_text(cfg['hazard']['detection'])}")
        print(f"    time to CRITICAL    {summary_text(cfg['hazard']['tta_s'], 1, 's')}")
        print(f"    nuisance CRITICAL   {rate_text(cfg['nuisance']['false_alarm'])}")
        print(f"    nuisance ELEVATED   {rate_text(cfg['nuisance']['elevated'])}")
        fid = cfg["fidelity"]
        if fid["node_transitions"]:
            print(f"    fidelity            {fid['matched']}/{fid['node_transitions']} node transitions reproduced")
    for skipped in doc["skipped_configs"]:
        print(f"\n  not replayed: {skipped['name']}: {skipped['reason']}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Replay FireX captures through the fusion engine and score them against marked trials.")
    ap.add_argument("captures", nargs="*", help="capture CSV files (each with its .events.jsonl beside it)")
    ap.add_argument("--labels", help="CSV of trials: capture,class,name,start_s,end_s (replaces marks for those captures)")
    ap.add_argument("--config", action="append", default=[], metavar="fx_key=value", help="base setting, repeatable")
    ap.add_argument("--sweep", action="append", default=[], metavar="fx_key=v1,v2", help="values to compare, repeatable")
    ap.add_argument("--grace-s", type=float, default=60.0, help="seconds after a trial ends that still count as its window")
    ap.add_argument("--out", default="reports/replay", help="output path without extension")
    ap.add_argument("--demo", action="store_true", help="SYNTHETIC: replay captures generated from the scenarios")
    ap.add_argument("--demo-dir", help="with --demo: keep the generated captures here")
    args = ap.parse_args()

    try:
        base = dict(fxreplay.parse_assignment(a) for a in args.config)
        sweeps = [fxreplay.parse_sweep(s) for s in (args.sweep or (DEMO_SWEEPS if args.demo else []))]
        configs = fxreplay.build_configs(base, sweeps)
    except ValueError as exc:
        print(f"replay: {exc}", file=sys.stderr)
        return 2
    if not available():
        print(f"replay: fusion engine not built at {binary_path()}: run `make -C firmware sim` or set FIREX_FUSION_CLI",
              file=sys.stderr)
        return 1

    temp = None
    try:
        if args.demo:
            if args.demo_dir:
                directory = Path(args.demo_dir)
            else:
                temp = tempfile.TemporaryDirectory(prefix="firex-replay-demo-")
                directory = Path(temp.name)
            paths = demo_captures(directory) + expand(args.captures)
            print(f"  {len(paths)} SYNTHETIC captures generated from the scenarios in {directory}")
        else:
            paths = expand(args.captures)
        if not paths:
            print("replay: no captures given (or --demo)", file=sys.stderr)
            return 2

        labels = fxreplay.load_labels(args.labels) if args.labels else None
        captures = [fxreplay.load_capture(p, labels=fxreplay.labels_for(labels, p)) for p in paths]
        doc = fxreplay.replay(captures, configs, args.grace_s, progress=lambda line: print(f"  {line}"))
        if args.demo:
            doc["synthetic"] = True
    except (OSError, ValueError, RuntimeError, EngineNotBuilt) as exc:
        print(f"replay: {exc}", file=sys.stderr)
        return 2
    finally:
        if temp is not None:
            temp.cleanup()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    json_path = out.with_name(out.name + ".json")
    md_path = out.with_name(out.name + ".md")
    raw = json.dumps(doc, indent=2) + "\n"
    json_path.write_text(raw, encoding="utf-8")
    inputs = report.load_inputs([str(json_path)])
    md_path.write_text(report.render(inputs, doc["generated_at"]), encoding="utf-8")

    headline(doc)
    for warning in doc["warnings"][:10]:
        print(f"  warning: {warning}")
    if len(doc["warnings"]) > 10:
        print(f"  ... and {len(doc['warnings']) - 10} more warnings in {json_path}")
    print(f"\n  wrote {json_path} and {md_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
