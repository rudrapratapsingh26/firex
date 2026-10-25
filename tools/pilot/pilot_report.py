#!/usr/bin/env python3
"""
The M5 pilot report, from the site gateways' pilot exports.

    # on each gateway:  npm run gateway:cli -- pilot export --from 2026-11-01 --to 2026-12-01 --out pilot-site-a
    python tools/pilot/pilot_report.py captures/pilot-site-a captures/pilot-site-b --out reports/pilot
    python tools/pilot/pilot_report.py --demo <dir> --out reports/pilot-demo      # two SYNTHETIC sites

Writes <out>.json and <out>.md. The Markdown leads with record integrity and the
M5 exit criteria, then the sites combined, then each site: units and
availability, incidents and their labels, every drill and live alert with who
was alerted, who acknowledged and how long each took, and the incident traces
replayed through the fusion engine when it is built (make -C firmware sim, or
FIREX_FUSION_CLI).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxpilot  # noqa: E402
from fxpilot import num, pct  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description="FireX M5 pilot report from site gateway exports.")
    ap.add_argument("sites", nargs="*", help="pilot export directories (or their pilot.json)")
    ap.add_argument("--out", default="reports/pilot", help="output path without extension")
    ap.add_argument("--demo", metavar="DIR", help="write two SYNTHETIC site exports into DIR and report on them")
    args = ap.parse_args()

    paths: list = list(args.sites)
    if args.demo:
        paths += fxpilot.write_demo(args.demo)
    if not paths:
        ap.error("give at least one site export directory, or --demo DIR")
    try:
        report = fxpilot.analyse(paths)
    except (OSError, ValueError) as exc:
        print(f"cannot read the exports: {exc}", file=sys.stderr)
        return 2

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    json_path, md_path = out.with_name(out.name + ".json"), out.with_name(out.name + ".md")
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    md_path.write_text(fxpilot.markdown(report), encoding="utf-8")

    c = report["combined"]
    milestone = report["criteria"][-1]
    tag = "SYNTHETIC " if report["synthetic"] else ""
    print(f"{tag}{len(report['sites'])} site(s), {c['units']} unit(s), {num(c['unit_days'], 1)} unit-days, "
          f"reporting {pct(c['availability'], 2)}; {c['incidents']} incident(s), labelled {c['labelled']['k']}/{c['labelled']['n']}; "
          f"false alarms {fxpilot.per100_text(c['false_alarms_per_100_unit_days'])}; {c['drills']} drill(s)")
    print(f"milestone: {milestone['status']}")
    for s in report["sites"]:
        if not s["traces"]["replayed"] and s["traces"]["count"]:
            print(f"  {s['site'].get('id')}: traces not replayed ({s['traces'].get('why_not')})")
    print(f"wrote {json_path} and {md_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
