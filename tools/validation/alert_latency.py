#!/usr/bin/env python3
"""
Alert-path latency on the bench: join a node capture with the mock endpoints' log
and report how long each step from threshold crossing to alert delivered took.

    python tools/validation/alert_latency.py --capture captures/m4-alerts.events.jsonl \\
        --csv captures/m4-alerts.csv --endpoints gateway-data/endpoints.jsonl \\
        --device 3a91c4... --ack-timeout-s 60 --out reports/alert-latency-bench
    python tools/validation/alert_latency.py --demo <dir>     # SYNTHETIC inputs and report

The capture comes from tools/capture/capture.py (or burnin.py) with the M4
host_unix_ms column; the endpoint log from the gateway's mock endpoints. Run the
capture, the gateway and the mock endpoints on the same machine so their clocks
agree, or pass --clock-offset-ms (gateway clock minus capture clock). Writes
<out>.json for tools/validation/report.py and <out>.md to read.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxlatency  # noqa: E402
import fxstats  # noqa: E402


def write(report: dict, out: str | Path) -> tuple[Path, Path]:
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    json_path, md_path = Path(f"{out}.json"), Path(f"{out}.md")
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    md_path.write_text(fxlatency.markdown(report), encoding="utf-8")
    return json_path, md_path


def main() -> int:
    ap = argparse.ArgumentParser(description="Alert-path latency from a node capture and the mock endpoints' log.")
    ap.add_argument("--capture", help="<capture>.events.jsonl")
    ap.add_argument("--csv", help="<capture>.csv, for mapping the node's clock (strongly recommended)")
    ap.add_argument("--endpoints", help="the mock endpoints' JSONL log")
    ap.add_argument("--device", help="only deliveries for this device ID")
    ap.add_argument("--ack-timeout-s", type=float, default=60.0, help="the gateway's acknowledgement window")
    ap.add_argument("--clock-offset-ms", type=int, default=0, help="gateway clock minus capture host clock")
    ap.add_argument("--services", help="comma-separated services every incident must reach (default: all in the log)")
    ap.add_argument("--out", default="reports/alert-latency-bench", help="output path without extension")
    ap.add_argument("--demo", metavar="DIR", help="write SYNTHETIC inputs into DIR and report on them")
    args = ap.parse_args()

    if args.demo:
        paths = fxlatency.demo_files(args.demo, ack_timeout_s=args.ack_timeout_s)
        args.capture, args.csv, args.endpoints = str(paths["events"]), str(paths["csv"]), str(paths["endpoints"])
        args.device = fxlatency.DEMO_DEVICE
        if args.out == "reports/alert-latency-bench":
            args.out = str(Path(args.demo) / "alert-latency-demo")
    if not (args.capture and args.endpoints):
        ap.error("--capture and --endpoints are required (or --demo DIR)")
    if not args.csv:
        print("warning: no --csv; the node clock is mapped from event lines only, which arrive later than stream lines",
              file=sys.stderr)

    report = fxlatency.analyse(args.capture, args.csv, args.endpoints, device=args.device,
                               ack_timeout_s=args.ack_timeout_s, clock_offset_ms=args.clock_offset_ms,
                               services=args.services.split(",") if args.services else None)
    json_path, md_path = write(report, args.out)
    total = report["segments"]["crossing_to_delivered_ms"]
    tag = "SYNTHETIC " if report["synthetic"] else ""
    print(f"{tag}{report['incidents']} incidents, {report['matched']} joined; crossing to delivered "
          f"{fxstats.summary_text({**total, **{k: None if total[k] is None else total[k] / 1000 for k in ('median', 'p90', 'max')}}, 2, 's')}; "
          f"re-alerts {report['realert']['observed_s']['n']} ({report['realert']['early']} early); "
          f"{len(report['failures'])} failures")
    print(f"wrote {json_path} and {md_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
