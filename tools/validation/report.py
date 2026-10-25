#!/usr/bin/env python3
"""
The M4 reliability report: every validation result in one Markdown document.

    python tools/validation/report.py reports/replay.json reports/burnin.json \\
        reports/escalation-dry-run.json reports/alert-latency.json reports/camera.json \\
        --out reports/m4-reliability.md

Inputs are the JSON files the M4 tools write, identified by their "kind":
replay (replay.py), burnin (burn-in analysis), alert_latency (the escalation
dry run, or the bench join), camera (frame checks). Any number of each, in any
order; missing kinds show as "no data".

Anything generated from synthetic input is labelled SYNTHETIC wherever its
numbers appear, and never satisfies an exit criterion. A synthetic number is a
check that the tooling works, not a measurement of the product.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxstats  # noqa: E402
from fxstats import md_table, num, pct, rate_text, summary_text  # noqa: E402

NUISANCE_MATRIX = ["cooking", "steam", "incense", "aerosol", "dust", "sunlight"]
SYNTHETIC_NOTE = "**SYNTHETIC: generated from made-up input. These numbers check the tooling; they are not measurements.**"

SEGMENTS = [
    ("crossing_to_critical_ms", "threshold crossing to CRITICAL (includes the dwell)"),
    ("critical_to_gateway_ms", "CRITICAL to the entry at the gateway"),
    ("gateway_to_delivered_ms", "gateway to alert delivered"),
    ("crossing_to_delivered_ms", "threshold crossing to alert delivered (end to end)"),
]


def load_inputs(paths: list[str]) -> list[dict]:
    inputs = []
    for path in paths:
        raw = Path(path).read_bytes()
        doc = json.loads(raw)
        if not isinstance(doc, dict) or "kind" not in doc:
            raise ValueError(f"{path}: not an M4 result (no 'kind')")
        inputs.append({"path": str(path), "sha256": hashlib.sha256(raw).hexdigest(), "doc": doc})
    return inputs


def _rate(block: dict | None, key: str) -> dict:
    r = (block or {}).get(key)
    return r if isinstance(r, dict) and "n" in r else fxstats.rate(0, 0)


def _summary(block: dict | None, key: str) -> dict:
    s = (block or {}).get(key)
    return s if isinstance(s, dict) and "n" in s else fxstats.summarize([])


def _real(inputs: list[dict], kind: str) -> list[dict]:
    return [i["doc"] for i in inputs if i["doc"].get("kind") == kind and not i["doc"].get("synthetic")]


def _any(inputs: list[dict], kind: str) -> list[dict]:
    return [i["doc"] for i in inputs if i["doc"].get("kind") == kind]


def _tag(doc: dict) -> str:
    return " (SYNTHETIC)" if doc.get("synthetic") else ""


def _yes(ok: object) -> str:
    return "yes" if ok is True else "NO" if ok is False else "-"


# --- replay ------------------------------------------------------------------------

def replay_section(inp: dict) -> list[str]:
    doc = inp["doc"]
    synthetic = bool(doc.get("synthetic"))
    verb = "simulated" if synthetic else "measured"
    out = [f"### Replay: `{Path(inp['path']).name}`{_tag(doc)}", ""]
    if synthetic:
        out += [SYNTHETIC_NOTE, ""]
    engine = doc.get("engine") or {}
    out.append(f"Engine `{engine.get('binary', '?')}`, sha256 `{engine.get('sha256') or 'unknown'}`. "
               f"Trial windows run to {num(doc.get('grace_s'), 0)} s after each trial ends.")
    out.append("")

    captures = doc.get("captures") or []
    if captures:
        rows = [[Path(str(c.get("path", "?"))).name, c.get("samples", "-"), num(c.get("duration_s"), 0, "s"),
                 num(c.get("period_ms"), 0, "ms"), c.get("gaps", "-"), c.get("boots", "-"), c.get("trials", "-"),
                 str(c.get("sha256", ""))[:16]] for c in captures]
        out += [md_table(["Capture", "Samples", "Duration", "Period", "Gaps", "Boots", "Trials", "sha256 (first 16)"], rows), ""]

    configs = doc.get("configs") or []
    if not configs:
        out += ["No configuration was replayed.", ""]
    for index, cfg in enumerate(configs):
        hazard, nuisance, bg = cfg.get("hazard") or {}, cfg.get("nuisance") or {}, cfg.get("background") or {}
        out += [f"#### {cfg.get('name', 'configuration')}{_tag(doc)}", ""]
        prefix = "Simulated, not measured: " if synthetic else ""
        out.append(f"- {prefix}hazard trials detected: {rate_text(_rate(hazard, 'detection'))}; "
                   f"missed: {rate_text(_rate(hazard, 'miss'))}.")
        out.append(f"- Time from the start of a hazard to CRITICAL ({verb}): {summary_text(_summary(hazard, 'tta_s'), 1, 's')}.")
        out.append(f"- Nuisance trials that reached CRITICAL: {rate_text(_rate(nuisance, 'false_alarm'))}; "
                   f"that reached ELEVATED only: {rate_text(_rate(nuisance, 'elevated'))}.")
        hours = bg.get("hours")
        out.append(f"- Background (outside every trial window): {num(hours, 2, 'h')}, "
                   f"{bg.get('false_alarms', 0)} transition(s) into CRITICAL"
                   + (f", {num(bg.get('per_24h'), 1)} per 24 h." if bg.get("per_24h") is not None else "."))
        fid = cfg.get("fidelity") or {}
        if fid.get("node_transitions"):
            out.append(f"- Fidelity: {fid.get('matched', 0)} of the node's {fid['node_transitions']} own state changes "
                       f"reproduced by the replay (it made {fid.get('replay_transitions', 0)} transitions)."
                       + (" Settings changed during the capture, so an exact match is not expected."
                          if fid.get("config_changes_in_capture") else ""))
        else:
            out.append("- Fidelity: the captures carry none of the node's own state changes to compare against.")
        out.append("")

        if index == 0:
            cat_rows = []
            for cat, block in (nuisance.get("by_category") or {}).items():
                cat_rows.append([cat, rate_text(_rate(block, "false_alarm")), rate_text(_rate(block, "elevated"))])
            if cat_rows:
                out += [f"Nuisance matrix{_tag(doc)}:", "",
                        md_table(["Category", "Reached CRITICAL", "Reached ELEVATED only"], cat_rows), ""]
            hz_rows = []
            for cat, block in (hazard.get("by_category") or {}).items():
                hz_rows.append([cat, rate_text(_rate(block, "detection")), summary_text(_summary(block, "tta_s"), 1, "s")])
            if hz_rows:
                out += [f"Hazards by category{_tag(doc)}:", "",
                        md_table(["Category", "Detected", "Time to CRITICAL"], hz_rows), ""]
            trials = cfg.get("trials") or []
            if trials:
                rows = [[Path(str(t.get("capture", "?"))).name, t.get("name", "?"), f"{t.get('class', '?')}/{t.get('category', '?')}",
                         t.get("outcome", "?"), t.get("max_state") or "-", num(t.get("tta_s"), 1, "s")] for t in trials]
                out += [f"Trials under {cfg.get('name', 'this configuration')}{_tag(doc)}:", "",
                        md_table(["Capture", "Trial", "Class/category", "Outcome", "Highest state", "Time to CRITICAL"], rows), ""]

    if len(configs) > 1:
        rows = []
        for cfg in configs:
            hazard, nuisance, bg = cfg.get("hazard") or {}, cfg.get("nuisance") or {}, cfg.get("background") or {}
            tta = _summary(hazard, "tta_s")
            rows.append([cfg.get("name", "?"), rate_text(_rate(hazard, "detection")), rate_text(_rate(hazard, "miss")),
                         f"{num(tta['median'], 1)} / {num(tta['p90'], 1)} s",
                         rate_text(_rate(nuisance, "false_alarm")), rate_text(_rate(nuisance, "elevated")),
                         num(bg.get("per_24h"), 1)])
        out += [f"Configurations compared{_tag(doc)}:", "",
                md_table(["Configuration", "Hazards detected", "Missed", "Time to CRITICAL median / p90",
                          "Nuisance CRITICAL", "Nuisance ELEVATED only", "Background CRITICAL per 24 h"], rows), ""]

        categories = []
        for cfg in configs:
            for cat in ((cfg.get("nuisance") or {}).get("by_category") or {}):
                if cat not in categories:
                    categories.append(cat)
        rows = [[cat] + [rate_text(_rate(((cfg.get("nuisance") or {}).get("by_category") or {}).get(cat), "false_alarm"))
                         for cfg in configs] for cat in categories]
        if rows:
            out += [f"Nuisance trials reaching CRITICAL, by category and configuration{_tag(doc)}:", "",
                    md_table(["Category"] + [cfg.get("name", "?") for cfg in configs], rows), ""]
        categories = []
        for cfg in configs:
            for cat, block in ((cfg.get("hazard") or {}).get("by_category") or {}).items():
                if cat not in categories and _rate(block, "detection")["n"]:
                    categories.append(cat)
        rows = []
        for cat in categories:
            row = [cat]
            for cfg in configs:
                block = ((cfg.get("hazard") or {}).get("by_category") or {}).get(cat) or {}
                row.append(f"{rate_text(_rate(block, 'detection'))}; median {num(_summary(block, 'tta_s')['median'], 1, 's')}")
            rows.append(row)
        if rows:
            out += [f"Hazards detected and median time to CRITICAL, by category and configuration{_tag(doc)}:", "",
                    md_table(["Category"] + [cfg.get("name", "?") for cfg in configs], rows), ""]

    for skipped in doc.get("skipped_configs") or []:
        out.append(f"- Not replayed: {skipped.get('name')}: {skipped.get('reason')}")
    warnings = doc.get("warnings") or []
    if warnings:
        out += ["Warnings:", ""] + [f"- {w}" for w in warnings[:50]]
        if len(warnings) > 50:
            out.append(f"- and {len(warnings) - 50} more")
    out.append("")
    return out


# --- burn-in ---------------------------------------------------------------------------

def burnin_section(inp: dict) -> list[str]:
    doc = inp["doc"]
    out = [f"### Burn-in: `{Path(inp['path']).name}`{_tag(doc)}", ""]
    if doc.get("synthetic"):
        out += [SYNTHETIC_NOTE, ""]
    inj = doc.get("injections") or {}
    resets = doc.get("resets") or {}
    heap = doc.get("heap") or {}
    tests = doc.get("self_tests") or {}
    overwrite = doc.get("log_overwrite") or {}
    out.append(f"- Duration: {num(doc.get('hours'), 1, 'h')}. Overall: {'PASS' if doc.get('pass') else 'FAIL'}.")
    out.append(f"- Injections sent: {inj.get('sent', 0)}; followed by CRITICAL: {rate_text(_rate(inj, 'detected'))}; "
               f"time to CRITICAL: {summary_text(_summary(inj, 'tta_s'), 1, 's')}.")
    missed = inj.get("missed") or []
    if missed:
        out.append(f"- Injections with no CRITICAL: {len(missed)}.")
    sources = doc.get("fault_sources")
    out.append(f"- Faults raised: {doc.get('faults_raised', 0)}"
               + (f" ({', '.join(f'{k} {v}' for k, v in sources.items())})" if isinstance(sources, dict) and sources else "") + ".")
    out.append(f"- Resets: watchdog {resets.get('watchdog', 0)}, panic {resets.get('panic', 0)}, "
               f"brownout {resets.get('brownout', 0)}, other {resets.get('other', 0)}; boots {doc.get('boots', 0)}.")
    out.append(f"- Self-tests: {tests.get('pass', 0)} passed, {tests.get('fail', 0)} failed. Ledger errors: {doc.get('ledger_errors', 0)}. "
               f"Log overwrites: {overwrite.get('mirrored', 0)} mirrored, {overwrite.get('unmirrored', 0)} not mirrored.")
    if heap:
        out.append(f"- Free heap: first {heap.get('first', '-')}, last {heap.get('last', '-')}, lowest {heap.get('min', '-')} bytes; "
                   f"trend {num(heap.get('slope_bytes_per_h'), 0, 'bytes/h')}.")
    criteria = doc.get("criteria") or []
    if criteria:
        out += ["", md_table(["Criterion", "Met", "Detail"],
                             [[c.get("name", "?"), _yes(c.get("ok")), c.get("detail", "")] for c in criteria])]
    out.append("")
    return out


# --- alert latency -----------------------------------------------------------------------

def latency_section(inp: dict) -> list[str]:
    doc = inp["doc"]
    source = doc.get("source", "?")
    title = {"dry_run": "Escalation dry run against mock endpoints", "bench": "Alert path on the bench"}.get(source, f"Alert latency ({source})")
    out = [f"### {title}: `{Path(inp['path']).name}`{_tag(doc)}", ""]
    if doc.get("synthetic"):
        out += [SYNTHETIC_NOTE, ""]
    if source == "dry_run":
        out += ["Simulated units and mock endpoints. Crossing to CRITICAL is only the dwell the dry run was given; "
                "the gateway segments and the re-alert timer are the gateway's own code, timed.", ""]
    out.append(f"Incidents: {doc.get('incidents', 0)}. Failures: {_failures(doc.get('failures'))}.")
    out.append("")
    segments = doc.get("segments") or {}
    rows = []
    for key, label in SEGMENTS:
        s = _summary(segments, key)
        rows.append([label, s["n"], num(s["min"], 0), num(s["median"], 0), num(s["p90"], 0), num(s["p99"], 0), num(s["max"], 0)])
    out += [md_table(["Segment (ms)", "n", "min", "median", "p90", "p99", "max"], rows), ""]
    services = doc.get("per_service") or {}
    if services:
        rows = []
        for svc, block in services.items():
            e2e, ack = _summary(block, "crossing_to_delivered_ms"), _summary(block, "ack_latency_s")
            rows.append([svc, block.get("delivered", "-"), f"{num(e2e['median'], 0)} / {num(e2e['p90'], 0)}",
                         f"{num(ack['median'], 1)} / {num(ack['p90'], 1)}"])
        out += [md_table(["Service", "Delivered", "End to end median / p90 (ms)", "Acknowledged after, median / p90 (s)"], rows), ""]
    realert = doc.get("realert") or {}
    if realert:
        observed = _summary(realert, "observed_s")
        out.append(f"Re-alert timer: configured {num(realert.get('configured_s'), 0, 's')}; observed {summary_text(observed, 2, 's')}; "
                   f"re-alerts sent before the window ended: {realert.get('early', 0)}.")
        out.append("")
    return out


def _failures(value: object) -> str:
    if isinstance(value, list):
        return str(len(value))
    if isinstance(value, dict):
        return ", ".join(f"{k} {v}" for k, v in value.items()) or "0"
    return str(value if value is not None else 0)


# --- camera ------------------------------------------------------------------------------

def camera_section(inp: dict) -> list[str]:
    doc = inp["doc"]
    out = [f"### Camera in the loop: `{Path(inp['path']).name}`{_tag(doc)}", ""]
    if doc.get("synthetic"):
        out += [SYNTHETIC_NOTE, ""]
    ref = doc.get("reference") or {}
    files = ref.get("files")
    out.append(f"Reference: {len(files) if isinstance(files, list) else files or 0} frame(s), mean luma {num(ref.get('mean_luma'), 1)}, "
               f"contrast {num(ref.get('contrast'), 1)}.")
    out.append("")
    frames = doc.get("frames") or []
    if frames:
        rows = [[Path(str(f.get("file", "?"))).name, f.get("bytes", "-"),
                 f"{f.get('width', '?')}x{f.get('height', '?')}", num(f.get("mean_luma"), 1), num(f.get("contrast"), 1),
                 num(f.get("sharpness"), 1), num(f.get("similarity"), 2), ", ".join(f.get("flags") or []) or "-"] for f in frames]
        out += [md_table(["Frame", "Bytes", "Size", "Mean luma", "Contrast", "Sharpness", "Similarity to reference", "Flags"], rows), ""]
    out += ["Whether a frame still shows the seat of the fire is a judgement made by looking at it; the metrics say "
            "where to look.", ""]
    return out


# --- exit criteria -------------------------------------------------------------------------

def _status(real_ok: bool | None, any_synthetic: bool, detail: str) -> tuple[str, str]:
    if real_ok is None:
        return ("no data (synthetic only)" if any_synthetic else "no data"), detail
    return ("met" if real_ok else "not met"), detail


def exit_criteria(inputs: list[dict]) -> list[list[str]]:
    rows = []
    replays = _real(inputs, "replay")
    syn_replay = bool(_any(inputs, "replay")) and not replays

    def first_configs() -> list[dict]:
        return [(d.get("configs") or [None])[0] for d in replays if d.get("configs")]

    trials = [t for cfg in first_configs() if cfg for t in (cfg.get("trials") or [])]
    if replays:
        ok = bool(trials)
        status = _status(ok, False, f"{len(trials)} trial(s) replayed from recorded captures" if ok else "recorded captures replayed, but no trials marked")
    else:
        status = _status(None, syn_replay, "")
    rows.append(["Replay harness: recorded traces in, transitions out, error rates in a report", *status])

    nuisance_cats = {t.get("category") for t in trials if t.get("class") == "nuisance"}
    if replays:
        missing = [c for c in NUISANCE_MATRIX if c not in nuisance_cats]
        status = _status(not missing, False, "all six categories trialled" if not missing else f"missing: {', '.join(missing)}")
    else:
        status = _status(None, syn_replay, "")
    rows.append(["Nuisance matrix: cooking, steam, incense, aerosol, dust, sunlight on the flame sensor", *status])

    hazard_cats = {t.get("category") for t in trials if t.get("class") == "hazard"}
    if replays:
        needs = {"gas": {"gas"}, "smoke": {"smoke", "smoulder"}, "flame": {"flame"}}
        missing = [k for k, cats in needs.items() if not hazard_cats & cats]
        if not any(t.get("class") == "nuisance" for t in trials):
            missing.append("nuisance trials")
        status = _status(not missing, False, "false-positive rate, miss rate and time to alarm measured" if not missing
                         else f"missing: {', '.join(missing)}")
    else:
        status = _status(None, syn_replay, "")
    rows.append(["Real gas, smoke and flame exposures: false-positive rate, miss rate, time to alarm", *status])

    if replays:
        most = max(len(d.get("configs") or []) for d in replays)
        status = _status(most > 1, False, f"{most} configuration(s) compared on recorded data; the chosen values are the team's decision")
    else:
        status = _status(None, syn_replay, "")
    rows.append(["Rules tuned against the recorded trials", *status])

    burnins, syn_burnin = _real(inputs, "burnin"), bool(_any(inputs, "burnin"))
    if burnins:
        best = max(burnins, key=lambda d: d.get("hours") or 0)
        hours = best.get("hours") or 0
        sent = (best.get("injections") or {}).get("sent", 0)
        status = _status(hours >= 72 and bool(sent), False, f"{num(hours, 1, 'h')}, {sent} injection(s)")
        rows.append(["72-hour burn-in with scheduled injections", *status])
        resets = best.get("resets") or {}
        faults, dogs = best.get("faults_raised", 0) or 0, resets.get("watchdog", 0) or 0
        rows.append(["Zero FAULT entries and zero watchdog resets",
                     *_status(faults == 0 and dogs == 0 and hours >= 72, False,
                              f"{faults} fault(s), {dogs} watchdog reset(s) in {num(hours, 1, 'h')}")])
    else:
        rows.append(["72-hour burn-in with scheduled injections", *_status(None, syn_burnin, "")])
        rows.append(["Zero FAULT entries and zero watchdog resets", *_status(None, syn_burnin, "")])

    cameras = _real(inputs, "camera")
    rows.append(["Camera in the loop: smoke build-up and an unlit room",
                 *(_status(bool(cameras), False, f"{sum(len(d.get('frames') or []) for d in cameras)} frame(s) checked; "
                                                 "the verdict is read from the frames") if cameras
                   else _status(None, bool(_any(inputs, "camera")), ""))])

    dry = [d for d in _real(inputs, "alert_latency") if d.get("source") == "dry_run"]
    syn_dry = any(d.get("source") == "dry_run" for d in _any(inputs, "alert_latency"))
    if dry:
        observed = sum(_summary(d.get("realert"), "observed_s")["n"] for d in dry)
        early = sum((d.get("realert") or {}).get("early", 0) or 0 for d in dry)
        delivered = sum(_summary(d.get("segments"), "gateway_to_delivered_ms")["n"] for d in dry)
        status = _status(bool(delivered) and bool(observed) and early == 0, False,
                         f"{delivered} delivery latency sample(s), {observed} re-alert(s) timed, {early} early")
    else:
        status = _status(None, syn_dry, "")
    rows.append(["Escalation dry runs against mock endpoints: alert latency and the re-alert timer", *status])

    bench = [d for d in _real(inputs, "alert_latency") if d.get("source") == "bench"]
    syn_bench = any(d.get("source") == "bench" for d in _any(inputs, "alert_latency"))
    if bench:
        n = sum(_summary(d.get("segments"), "crossing_to_delivered_ms")["n"] for d in bench)
        status = _status(n > 0, False, f"{n} end-to-end sample(s)")
    else:
        status = _status(None, syn_bench, "")
    rows.append(["Alert-path latency distribution, threshold crossing to alert delivered", *status])

    milestone = all(rows[i][1] == "met" for i in (0, 2, 8))
    rows.append(["Milestone: reliability report with measured error rates and the alert-path latency distribution",
                 "met" if milestone else "not met", "needs the replay, exposure and alert-path rows above"])
    return rows


# --- document ------------------------------------------------------------------------------

def render(inputs: list[dict], generated_at: str | None = None) -> str:
    generated_at = generated_at or datetime.now(timezone.utc).isoformat(timespec="seconds")
    synthetic = any(i["doc"].get("synthetic") for i in inputs)
    out = ["# FireX M4 reliability report", ""]
    if synthetic:
        out += ["> **SYNTHETIC INPUT PRESENT.** Sections marked SYNTHETIC were generated from made-up traces or "
                "simulated units. They show that the tooling works. They are not measurements of FireX, and they "
                "satisfy no exit criterion.", ""]
    out += [f"Generated {generated_at} by `tools/validation/report.py`. Every number below is "
            "only as good as the captures and runs listed under Inputs.", ""]

    out += ["## Inputs", ""]
    if inputs:
        out += [md_table(["File", "Kind", "Synthetic", "sha256"],
                         [[Path(i["path"]).name, i["doc"].get("kind"), "yes" if i["doc"].get("synthetic") else "no", i["sha256"]]
                          for i in inputs]), ""]
    else:
        out += ["None.", ""]

    out += ["## M4 exit criteria", "", md_table(["Criterion", "Status", "Detail"], exit_criteria(inputs)), ""]

    sections = [
        ("## Detection and false alarms (replay)", "replay", replay_section),
        ("## Burn-in", "burnin", burnin_section),
        ("## Alert-path latency", "alert_latency", latency_section),
        ("## Camera in the loop", "camera", camera_section),
    ]
    for title, kind, fn in sections:
        out += [title, ""]
        chosen = [i for i in inputs if i["doc"].get("kind") == kind]
        if kind == "alert_latency":
            chosen.sort(key=lambda i: 0 if i["doc"].get("source") == "dry_run" else 1)
        if not chosen:
            out += ["No data.", ""]
        for inp in chosen:
            out += fn(inp)

    unknown = [i for i in inputs if i["doc"].get("kind") not in {k for _, k, _ in sections}]
    if unknown:
        out += ["## Not understood", ""] + [f"- `{Path(i['path']).name}`: kind {i['doc'].get('kind')!r}" for i in unknown] + [""]
    return "\n".join(out).rstrip() + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble the FireX M4 reliability report from validation results.")
    ap.add_argument("inputs", nargs="+", help="JSON results: replay, burnin, alert_latency, camera")
    ap.add_argument("--out", default="reports/m4-reliability.md")
    args = ap.parse_args()
    try:
        inputs = load_inputs(args.inputs)
    except (OSError, ValueError) as exc:
        print(f"report: {exc}", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(inputs), encoding="utf-8")
    real = [i for i in inputs if not i["doc"].get("synthetic")]
    print(f"wrote {out} from {len(inputs)} input(s), {len(inputs) - len(real)} synthetic")
    return 0


if __name__ == "__main__":
    sys.exit(main())
