"""
The M5 pilot report: what two live sites recorded, and the escalation record
that shows who was alerted, who acknowledged, and how long each took.

Input is what each site gateway writes with `gateway:cli pilot export --from
--to --out <dir>`: <dir>/pilot.json, the unit and site ledger bundles it names,
and <dir>/traces/<sha>.csv and .events.jsonl, the incident traces in the M4
capture format. tools/pilot/pilot_report.py is the command line; this module
does the arithmetic, so tools/pilot/test_fxpilot.py checks it in CI.

What the report says, and what it cannot:

    availability   1 - time a unit was not reporting / time it was installed in the period,
                   gaps merged and clipped to the period; the alarm itself never depends on it
    false alarms   incidents the site contact labelled nuisance, per 100 installed unit-days,
                   with the exact Poisson interval
    escalation     every drill and every live incident: each recipient, when the alert was
                   sent and delivered, who acknowledged and after how long, timeouts and
                   re-alerts; latency distributions for drills and live incidents kept apart
    traces         replayed through the compiled engine when it is built; a trace exists
                   only around an episode, so traces show how the rules behaved on episodes
                   the site labelled, never the miss rate of hazards that raised no score

Recipient numbers are masked again here, whatever the export says: a pilot
report is shared, and a station's phone number is not the report's to publish.

Standard library only.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "validation"))
import fxstats  # noqa: E402
from fxstats import md_table, num, pct, rate_text, summary_text  # noqa: E402

SERVICES = ["site", "fire", "police", "ambulance"]
LABELS = ["true", "nuisance", "test"]
DAY_MS = 86_400_000
SYNTHETIC_NOTE = "**SYNTHETIC: generated from made-up input. These numbers check the tooling; they are not measurements.**"
TRACE_NOTE = ("A trace exists only around an episode, a unit leaving NORMAL. Replaying traces shows how the rules "
              "behaved on the episodes the site labelled; it cannot measure the miss rate of a hazard that never "
              "raised a score, because no trace exists for one.")


# --- loading -----------------------------------------------------------------------

@dataclass
class Site:
    dir: Path
    path: Path
    sha256: str
    doc: dict

    @property
    def synthetic(self) -> bool:
        return bool(self.doc.get("synthetic"))

    @property
    def name(self) -> str:
        site = self.doc.get("site") or {}
        return str(site.get("name") or site.get("id") or self.dir.name)


def load_site(path: str | Path) -> Site:
    """A site export directory, or its pilot.json."""
    path = Path(path)
    file = path / "pilot.json" if path.is_dir() else path
    raw = file.read_bytes()
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{file}: not JSON ({exc})") from exc
    if not isinstance(doc, dict) or doc.get("kind") != "pilot_site":
        raise ValueError(f"{file}: not a site pilot export (kind must be pilot_site)")
    return Site(file.parent, file, hashlib.sha256(raw).hexdigest(), doc)


DIGITS = re.compile(r"\d")


def mask_recipient(value: object) -> str:
    """Keep the last four digits of anything that looks like a number; leave station codes alone."""
    text = "" if value is None else str(value)
    digits = DIGITS.findall(text)
    if len(digits) <= 4:
        return text
    last_mask = max(text.rfind("•"), text.rfind("*"))
    if last_mask >= 0 and len(DIGITS.findall(text[last_mask:])) <= 4:
        return text  # masked already by the gateway: country code in front, four digits after
    keep = len(digits) - 4
    out, seen = [], 0
    for ch in text:
        if ch.isdigit():
            out.append("•" if seen < keep else ch)
            seen += 1
        else:
            out.append(ch)
    return "".join(out)


# --- time ----------------------------------------------------------------------------

def iso_ms(ms: float | None) -> str:
    if ms is None:
        return "-"
    return datetime.fromtimestamp(ms / 1000.0, timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")


def merged_gap_ms(gaps: list[dict], start_ms: int, end_ms: int) -> int:
    """Total of the gaps inside [start_ms, end_ms], overlaps counted once; an open gap runs to end_ms."""
    spans = []
    for gap in gaps or []:
        s = gap.get("start_ms")
        if s is None:
            continue
        e = gap.get("end_ms")
        e = end_ms if e is None else e
        s, e = max(int(s), start_ms), min(int(e), end_ms)
        if e > s:
            spans.append((s, e))
    spans.sort()
    total, current = 0, None
    for s, e in spans:
        if current is None or s > current[1]:
            if current:
                total += current[1] - current[0]
            current = [s, e]
        else:
            current[1] = max(current[1], e)
    if current:
        total += current[1] - current[0]
    return total


# --- one site --------------------------------------------------------------------------

def unit_summary(unit: dict, period: dict) -> dict:
    from_ms, to_ms = int(period.get("from_ms") or 0), int(period.get("to_ms") or 0)
    provisioned = unit.get("provisioned_at")
    start = max(from_ms, int(provisioned)) if provisioned is not None else from_ms
    window = max(0, to_ms - start)
    gap = merged_gap_ms(unit.get("gaps") or [], start, to_ms)
    # Each gap's length inside the period, an open one running to its end, rather than what the export counted so far.
    gaps = sorted(({"start_ms": g.get("start_ms"), "end_ms": g.get("end_ms"), "open": g.get("end_ms") is None,
                    "seconds": merged_gap_ms([g], start, to_ms) / 1000.0} for g in unit.get("gaps") or [] if g.get("start_ms") is not None),
                  key=lambda g: -g["seconds"])
    updates = unit.get("fw_updates") or []
    by_phase = {phase: sum(1 for u in updates if u.get("phase") == phase)
                for phase in ("accepted", "downloaded", "confirmed", "failed", "rolled_back")}
    tests = unit.get("self_tests") or {}
    return {
        "id": unit.get("id"),
        "label": unit.get("label") or str(unit.get("id", ""))[:8],
        "installed_days": window / DAY_MS,
        "gap_s": gap / 1000.0,
        "availability": (1.0 - gap / window) if window else None,
        "longest_gap": gaps[0] if gaps else None,
        "gaps": len(unit.get("gaps") or []),
        "fw_versions": list(unit.get("fw_versions") or []),
        "fw_updates": by_phase,
        "fw_problems": [u for u in updates if u.get("phase") in ("failed", "rolled_back")],
        "self_tests": {"pass": int(tests.get("pass") or 0), "fail": int(tests.get("fail") or 0)},
        "faults_raised": int(unit.get("faults_raised") or 0),
        "elevated_episodes": int(unit.get("elevated_episodes") or 0),
    }


def alert_row(alert: dict, mode: str) -> dict:
    sent, delivered, acked = alert.get("sent_at_ms"), alert.get("delivered_at_ms"), alert.get("acked_at_ms")
    return {
        "service": alert.get("service"),
        "channel": alert.get("channel"),
        "recipient": mask_recipient(alert.get("recipient_masked")),
        "mode": mode,
        "sent_at_ms": sent,
        "delivered_after_ms": (delivered - sent) if delivered is not None and sent is not None else None,
        "acked_by": alert.get("acked_by") if acked is not None else None,
        "latency_s": alert.get("latency_s") if acked is not None else None,
        "timeouts": int(alert.get("timeouts") or 0),
        "re_alerts": int(alert.get("re_alerts") or 0),
        "failed": bool(alert.get("failed")),
    }


def record_complete(rows: list[dict]) -> bool:
    """A drill record that answers the milestone: every recipient's send is recorded, every one
    either acknowledged or timed out on the record, and somebody acknowledged."""
    return (bool(rows) and all(r["sent_at_ms"] is not None for r in rows)
            and all(r["latency_s"] is not None or r["timeouts"] > 0 for r in rows)
            and any(r["latency_s"] is not None for r in rows))


def latency_by_service(groups: list[list[dict]]) -> dict:
    out = {}
    rows = [r for group in groups for r in group]
    for service in SERVICES + sorted({str(r["service"]) for r in rows} - set(SERVICES)):
        mine = [r for r in rows if r["service"] == service]
        if not mine:
            continue
        acked = [r for r in mine if r["latency_s"] is not None]
        out[service] = {
            "alerted": len(mine),
            "acknowledged": fxstats.rate(len(acked), len(mine)),
            "ack_latency_s": fxstats.summarize([float(r["latency_s"]) for r in acked]),
            "delivery_ms": fxstats.summarize([float(r["delivered_after_ms"]) for r in mine if r["delivered_after_ms"] is not None]),
        }
    return out


def summarize_site(site: Site) -> dict:
    doc = site.doc
    period = doc.get("period") or {}
    registry = doc.get("registry") or {}
    modes = {svc: (block or {}).get("mode", "drill") for svc, block in (registry.get("services") or {}).items()}
    units = [unit_summary(u, period) for u in doc.get("units") or []]
    labels = {u["id"]: u["label"] for u in units}
    unit_days = sum(u["installed_days"] for u in units)
    gap_s = sum(u["gap_s"] for u in units)

    incidents = doc.get("incidents") or []
    counts = {label: sum(1 for i in incidents if i.get("label") == label) for label in LABELS}
    counts["unlabelled"] = sum(1 for i in incidents if i.get("label") is None)
    to_label = [(i["labelled_at_ms"] - i["start_ts"] * 1000) / 1000.0 for i in incidents
                if i.get("labelled_at_ms") is not None and i.get("start_ts") is not None
                and i.get("ts_src") in ("gateway", "ntp")]

    def brief(i: dict) -> dict:
        return {"id": i.get("id"), "unit": labels.get(i.get("unit"), str(i.get("unit", ""))[:8]),
                "start_ts": i.get("start_ts"), "ts_src": i.get("ts_src"), "reason": i.get("reason") or [],
                "label": i.get("label"), "late": bool(i.get("late")), "escalation_state": i.get("escalation_state")}

    drills = []
    for d in doc.get("drills") or []:
        rows = [alert_row(a, "drill") for a in d.get("alerts") or []]
        drills.append({"id": d.get("id"), "started_at_ms": d.get("started_at_ms"), "finished_at_ms": d.get("finished_at_ms"),
                       "by": d.get("by"), "services": d.get("services") or [], "alerts": rows,
                       "complete": record_complete(rows)})
    live = []
    for i in incidents:
        rows = [alert_row(a, modes.get(str(a.get("service")), "live" if a.get("service") == "site" else "drill"))
                for a in i.get("alerts") or []]
        not_alerted = [svc for svc in SERVICES if svc != "site" and modes.get(svc, "drill") == "drill"]
        live.append({**brief(i), "alerts": rows, "not_alerted_drill_mode": not_alerted})

    ledger = doc.get("site_ledger") or {}
    bundles = doc.get("unit_bundles") or []
    integrity = [{"record": "site ledger", "verify": str(ledger.get("verify") or "not exported"),
                  "entries": ledger.get("entries")}]
    integrity += [{"record": f"unit {labels.get(b.get('unit'), str(b.get('unit', ''))[:8])}", "verify": str(b.get("verify") or "not exported"),
                   "entries": b.get("entries")} for b in bundles]
    for item in integrity:
        item["ok"] = item["verify"].startswith("PASS")

    services = {svc: {"mode": (block or {}).get("mode", "drill"), "agreement": (block or {}).get("agreement")}
                for svc, block in (registry.get("services") or {}).items()}

    return {
        "path": str(site.path),
        "sha256": site.sha256,
        "synthetic": site.synthetic,
        "site": doc.get("site") or {},
        "period": {"from_ms": period.get("from_ms"), "to_ms": period.get("to_ms"),
                   "days": ((period.get("to_ms") or 0) - (period.get("from_ms") or 0)) / DAY_MS},
        "units": units,
        "unit_days": unit_days,
        "availability": (1.0 - gap_s * 1000.0 / (unit_days * DAY_MS)) if unit_days else None,
        "longest_gaps": sorted(({"unit": u["label"], **u["longest_gap"]} for u in units if u["longest_gap"]),
                               key=lambda g: -(float(g.get("seconds") or 0)))[:5],
        "incidents": {
            "total": len(incidents),
            "by_label": counts,
            "labelled": fxstats.rate(len(incidents) - counts["unlabelled"], len(incidents)),
            "time_to_label_s": fxstats.summarize(to_label),
            "false_alarms_per_100_unit_days": fxstats.rate_per(counts["nuisance"], unit_days, per=100.0),
            "true_hazards": [brief(i) for i in incidents if i.get("label") == "true"],
            "late": [brief(i) for i in incidents if i.get("late")],
            "unlabelled": [brief(i) for i in incidents if i.get("label") is None],
        },
        "drills": drills,
        "live_incidents": live,
        "ack_latency": {"drills": latency_by_service([d["alerts"] for d in drills]),
                        "live": latency_by_service([i["alerts"] for i in live])},
        "services": services,
        "integrity": integrity,
        "traces": replay_traces(site),
    }


def replay_traces(site: Site) -> dict:
    entries = site.doc.get("traces") or []
    listed = [{"sha256": t.get("sha256"), "unit": t.get("unit"), "incident": t.get("incident"),
               "samples": t.get("samples"), "period_ms": t.get("period_ms"), "trigger": t.get("trigger")} for t in entries]
    result = {"count": len(entries), "listed": listed, "replayed": False, "note": TRACE_NOTE}
    if not entries:
        result["why_not"] = "no traces in this export"
        return result
    try:
        import fxreplay  # noqa: E402  (puts tools/simulator on the path for fxengine)
        import fxengine  # noqa: E402
    except ImportError as exc:  # pragma: no cover - the validation tools are in the repository
        result["why_not"] = f"replay tools not importable: {exc}"
        return result
    if not fxengine.available():
        result["why_not"] = "fusion engine not built (make -C firmware sim, or set FIREX_FUSION_CLI): traces listed, not replayed"
        return result
    captures, missing = [], []
    for t in entries:
        csv_path = site.dir / str(t.get("csv") or f"traces/{t.get('sha256')}.csv")
        events = site.dir / str(t["events"]) if t.get("events") else None
        if not csv_path.exists():
            missing.append(str(csv_path.name))
            continue
        try:
            captures.append(fxreplay.load_capture(csv_path, events))
        except ValueError as exc:
            missing.append(f"{csv_path.name}: {exc}")
    result["missing"] = missing
    if not captures:
        result["why_not"] = "none of the listed trace files could be read"
        return result
    doc = fxreplay.replay(captures, fxreplay.build_configs({}, []))
    if not doc["configs"]:
        result["why_not"] = "the node's default settings were refused by the engine"
        return result
    cfg = doc["configs"][0]
    fid = {k: cfg["fidelity"][k] for k in ("node_transitions", "matched", "replay_transitions")}
    result.update({
        "replayed": True,
        "synthetic": doc["synthetic"],
        "engine_sha256": doc["engine"]["sha256"],
        "fidelity": fid,
        "hazard_episodes": cfg["hazard"]["detection"],
        "nuisance_episodes": cfg["nuisance"]["false_alarm"],
        "trials": [{k: t[k] for k in ("capture", "class", "name", "max_state", "outcome", "tta_s")} for t in cfg["trials"]],
        "warnings": doc["warnings"][:10],
    })
    return result


# --- across sites ------------------------------------------------------------------------

def combine(summaries: list[dict]) -> dict:
    real = [s for s in summaries if not s["synthetic"]]
    pool = real or summaries
    unit_days = sum(s["unit_days"] for s in pool)
    gap_days = sum(sum(u["gap_s"] for u in s["units"]) / 86_400.0 for s in pool)
    nuisance = sum(s["incidents"]["by_label"]["nuisance"] for s in pool)
    incidents = sum(s["incidents"]["total"] for s in pool)
    unlabelled = sum(s["incidents"]["by_label"]["unlabelled"] for s in pool)
    fid = {"node_transitions": 0, "matched": 0, "replay_transitions": 0}
    for s in pool:
        for key in fid:
            fid[key] += (s["traces"].get("fidelity") or {}).get(key, 0)
    return {
        "synthetic": not real and bool(summaries),
        "sites": [s["site"].get("id") for s in pool],
        "units": sum(len(s["units"]) for s in pool),
        "unit_days": unit_days,
        "availability": (1.0 - gap_days / unit_days) if unit_days else None,
        "incidents": incidents,
        "by_label": {label: sum(s["incidents"]["by_label"][label] for s in pool) for label in LABELS + ["unlabelled"]},
        "labelled": fxstats.rate(incidents - unlabelled, incidents),
        "false_alarms_per_100_unit_days": fxstats.rate_per(nuisance, unit_days, per=100.0),
        "drills": sum(len(s["drills"]) for s in pool),
        "ack_latency": {"drills": latency_by_service([d["alerts"] for s in pool for d in s["drills"]]),
                        "live": latency_by_service([i["alerts"] for s in pool for i in s["live_incidents"]])},
        "trace_fidelity": fid,
    }


def _status(real_ok: bool | None, any_synthetic: bool, detail: str) -> dict:
    if real_ok is None:
        return {"status": "no data (synthetic only)" if any_synthetic else "no data", "detail": detail}
    return {"status": "met" if real_ok else "not met", "detail": detail}


def criteria(summaries: list[dict]) -> list[dict]:
    real = [s for s in summaries if not s["synthetic"]]
    synthetic = any(s["synthetic"] for s in summaries)
    rows = []

    with_data = [s for s in real if s["units"] and s["period"]["days"] > 0]
    rows.append({"name": "Two live sites with data over the period",
                 **_status(len(with_data) >= 2 if real else None, synthetic,
                           f"{len(with_data)} real site(s) with units and a period")})

    if real:
        per_site = [(s["site"].get("name") or s["site"].get("id"), any(d["complete"] for d in s["drills"])) for s in real]
        ok = all(done for _, done in per_site)
        detail = "; ".join(f"{name}: {'drill recorded' if done else 'no complete drill record'}" for name, done in per_site)
    else:
        ok, detail = None, ""
    rows.append({"name": "A logged escalation drill per site: who was alerted, who acknowledged, how long each took",
                 **_status(ok, synthetic, detail)})

    if real:
        total = sum(s["incidents"]["total"] for s in real)
        unlabelled = sum(s["incidents"]["by_label"]["unlabelled"] for s in real)
        ok, detail = unlabelled == 0, f"{total - unlabelled}/{total} incident(s) labelled by the site"
    else:
        ok, detail = None, ""
    rows.append({"name": "Every incident labelled by the site contact", **_status(ok, synthetic, detail)})

    if real:
        bad = [f"{s['site'].get('id')} {i['record']}: {i['verify']}" for s in real for i in s["integrity"] if not i["ok"]]
        ok, detail = not bad, "; ".join(bad) or "site ledger and every unit bundle PASS"
    else:
        ok, detail = None, ""
    rows.append({"name": "Site ledger and unit bundles verify", **_status(ok, synthetic, detail)})

    if real:
        shown = [s for s in real if s["units"] and s["availability"] is not None]
        ok = len(shown) == len(real)
        detail = "; ".join(f"{s['site'].get('id')}: {pct(s['availability'], 1)} over {num(s['unit_days'], 1)} unit-days" for s in shown)
    else:
        ok, detail = None, ""
    rows.append({"name": "Units reporting, with availability shown", **_status(ok, synthetic, detail)})

    statuses = [r["status"] for r in rows]
    if all(st == "met" for st in statuses):
        milestone = {"status": "met", "detail": "every row above is met"}
    elif any(st == "not met" for st in statuses):
        milestone = {"status": "not met", "detail": "needs every row above"}
    else:
        milestone = _status(None, synthetic, "needs every row above")
    rows.append({"name": "Milestone: pilot data from two live sites and a logged escalation drill", **milestone})
    return rows


def analyse(paths: list[str | Path], generated_at: str | None = None) -> dict:
    sites = [load_site(p) for p in paths]
    summaries = [summarize_site(s) for s in sites]
    return {
        "kind": "pilot",
        "synthetic": any(s.synthetic for s in sites),
        "generated_at": generated_at or datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "inputs": [{"path": str(s.path), "sha256": s.sha256, "synthetic": s.synthetic} for s in sites],
        "sites": summaries,
        "combined": combine(summaries),
        "criteria": criteria(summaries),
    }


# --- Markdown ---------------------------------------------------------------------------------

def per100_text(r: dict) -> str:
    if r["rate"] is None:
        return f"{r['k']} with no installed unit-days"
    lo, hi = r["ci95"]
    return (f"{r['k']} in {num(r['exposure'], 1)} unit-days: {num(r['rate'], 1)} per 100 unit-days "
            f"(95 % CI {num(lo, 1)}-{num(hi, 1)})")


def _when(ts: float | None, src: str | None) -> str:
    if ts is None:
        return "-"
    text = iso_ms(ts * 1000.0)
    return f"{text} (clock uncertain)" if src == "uncertain" else text


def _alert_table(rows: list[dict]) -> str:
    table = []
    for r in rows:
        ack = (f"{r['acked_by']} after {num(r['latency_s'], 0, 's')}" if r["latency_s"] is not None
               else "no acknowledgement")
        table.append([r["service"], r["channel"] or "-", r["recipient"] or "-", r["mode"], iso_ms(r["sent_at_ms"]),
                      "-" if r["delivered_after_ms"] is None else f"{r['delivered_after_ms']:.0f} ms",
                      ack, r["timeouts"], r["re_alerts"], "yes" if r["failed"] else "no"])
    return md_table(["Service", "Channel", "Recipient", "Mode", "Sent", "Delivered after", "Acknowledged",
                     "Timeouts", "Re-alerts", "Send failed"], table)


def _latency_table(block: dict) -> str:
    rows = [[svc, b["alerted"], rate_text(b["acknowledged"]), summary_text(b["ack_latency_s"], 0, "s"),
             summary_text(b["delivery_ms"], 0, "ms")] for svc, b in block.items()]
    return md_table(["Service", "Alerted", "Acknowledged", "Time to acknowledge", "Delivered after"], rows)


def site_section(s: dict) -> list[str]:
    tag = " (SYNTHETIC)" if s["synthetic"] else ""
    prefix = "Simulated, not measured: " if s["synthetic"] else ""
    site = s["site"]
    out = [f"## Site: {site.get('name') or site.get('id')}{tag}", ""]
    if s["synthetic"]:
        out += [SYNTHETIC_NOTE, ""]
    out += [f"Period {iso_ms(s['period']['from_ms'])} to {iso_ms(s['period']['to_ms'])} ({num(s['period']['days'], 1)} days). "
            f"{prefix}{len(s['units'])} unit(s), {num(s['unit_days'], 1)} installed unit-days, "
            f"reporting {pct(s['availability'], 2)} of that time.", ""]

    out += ["### Units", ""]
    rows = []
    for u in s["units"]:
        g = u["longest_gap"]
        rows.append([u["label"], num(u["installed_days"], 1), pct(u["availability"], 2),
                     "-" if not g else (f"{num(g['seconds'] / 60.0, 0)} min from {iso_ms(g['start_ms'])}"
                                        f"{' (still open)' if g['open'] else ''}"),
                     " → ".join(u["fw_versions"]) or "-",
                     f"{u['fw_updates']['confirmed']} / {u['fw_updates']['failed']} / {u['fw_updates']['rolled_back']}",
                     f"{u['self_tests']['pass']} / {u['self_tests']['fail']}", u["faults_raised"], u["elevated_episodes"]])
    out += [md_table(["Unit", "Installed days", "Reporting", "Longest gap", "Firmware", "Updates confirmed / failed / rolled back",
                      "Self-tests pass / fail", "Faults", "ELEVATED episodes"], rows), ""]
    problems = [(u["label"], p) for u in s["units"] for p in u["fw_problems"]]
    if problems:
        out += ["Firmware updates that did not stay installed:", ""]
        out += [f"- {label}: {p.get('phase')} {p.get('from_version') or '?'} → {p.get('version') or '?'}"
                f"{(': ' + str(p['reason'])) if p.get('reason') else ''}" for label, p in problems]
        out.append("")

    inc = s["incidents"]
    by = inc["by_label"]
    out += ["### Incidents", "",
            f"{prefix}{inc['total']} critical alarm(s): {by['true']} real hazard, {by['nuisance']} nuisance, "
            f"{by['test']} test, {by['unlabelled']} not labelled. Labelled by the site: {rate_text(inc['labelled'])}; "
            f"time to label {summary_text(inc['time_to_label_s'], 0, 's')}.", "",
            f"False alarms: {per100_text(inc['false_alarms_per_100_unit_days'])}.", ""]
    for title, key in (("Real hazards", "true_hazards"), ("Late: reached the gateway after the unit was offline; only the site was alerted", "late"),
                       ("Not labelled", "unlabelled")):
        if inc[key]:
            out += [f"{title}:", ""]
            out += [md_table(["Incident", "Unit", "Critical at", "Signals", "Label", "Escalation"],
                             [[i["id"], i["unit"], _when(i["start_ts"], i["ts_src"]), " + ".join(i["reason"]) or "-",
                               i["label"] or "-", i["escalation_state"] or "-"] for i in inc[key]]), ""]

    out += ["### Escalation drills", ""]
    if not s["drills"]:
        out += ["No drill was logged at this site in the period.", ""]
    for d in s["drills"]:
        out += [f"Drill {d['id']}, started {iso_ms(d['started_at_ms'])} by {d['by'] or '?'}"
                f"{'' if d['complete'] else ' (record incomplete: a send, an acknowledgement or a timeout is missing)'}:", "",
                _alert_table(d["alerts"]), ""]

    out += ["### Live incidents: who was alerted", ""]
    alerted = [i for i in s["live_incidents"] if i["alerts"]]
    if not alerted:
        out += ["No live incident alerted anyone in the period.", ""]
    for i in alerted:
        withheld = f" Not alerted, still in drill mode: {', '.join(i['not_alerted_drill_mode'])}." if i["not_alerted_drill_mode"] else ""
        out += [f"Incident {i['id']} at {i['unit']}, {_when(i['start_ts'], i['ts_src'])}, labelled {i['label'] or 'not yet'}"
                f"{', late' if i['late'] else ''}.{withheld}", "", _alert_table(i["alerts"]), ""]

    out += ["### Time to acknowledge", ""]
    for title, key in (("Drills", "drills"), ("Live incidents", "live")):
        block = s["ack_latency"][key]
        out += [f"{title}:", "", _latency_table(block) if block else "No alerts.", ""]

    out += ["### Services and agreements", ""]
    out += [md_table(["Service", "Mode", "Agreement"],
                     [[svc, b["mode"], (f"{b['agreement'].get('reference')} ({b['agreement'].get('date')})"
                                        if b.get("agreement") else ("-" if svc == "site" else "none recorded"))]
                      for svc, b in s["services"].items()]), ""]

    out += trace_lines(s["traces"])
    return out


def trace_lines(t: dict) -> list[str]:
    out = ["### Traces", "", TRACE_NOTE, ""]
    if not t["replayed"]:
        out += [f"{t['count']} trace(s). Not replayed: {t.get('why_not', 'unknown reason')}.", ""]
        return out
    fid = t["fidelity"]
    tag = " (SYNTHETIC)" if t.get("synthetic") else ""
    out += [f"{t['count']} trace(s) replayed with the node's default settings{tag}. The replay reproduced "
            f"{fid['matched']}/{fid['node_transitions']} of the units' own state changes.", "",
            f"Episodes the site labelled a real hazard that the replay took to CRITICAL: {rate_text(t['hazard_episodes'])}. "
            f"Episodes labelled nuisance that it took to CRITICAL: {rate_text(t['nuisance_episodes'])}.", ""]
    if t["trials"]:
        out += [md_table(["Trace", "Label", "Episode", "Highest state", "Outcome"],
                         [[tr["capture"], tr["class"], tr["name"], tr["max_state"] or "-", tr["outcome"]] for tr in t["trials"]]), ""]
    if t.get("missing"):
        out += [f"Listed but not read: {', '.join(t['missing'])}.", ""]
    return out


def markdown(report: dict) -> str:
    out = ["# FireX M5 pilot report", ""]
    if report["synthetic"]:
        out += ["> **SYNTHETIC INPUT PRESENT.** Sections marked SYNTHETIC come from made-up site exports. They show that "
                "the tooling works. They are not pilot data, and they satisfy no exit criterion.", ""]
    out += [f"Generated {report['generated_at']} by `tools/pilot/pilot_report.py`. Every number below is only as good as "
            "the site exports listed under Inputs.", ""]

    bad = [(s["site"].get("name") or s["site"].get("id"), i) for s in report["sites"] for i in s["integrity"] if not i["ok"]]
    out += ["## Record integrity", ""]
    if bad:
        out += ["**Records that do not verify:**", ""]
        out += [f"- {name}, {i['record']}: {i['verify']}" for name, i in bad]
    else:
        out.append("The site ledger and every unit bundle in every export verify PASS.")
    out.append("")

    out += ["## Inputs", "",
            md_table(["Export", "Site", "Synthetic", "sha256"],
                     [[Path(i["path"]).parent.name, s["site"].get("name") or s["site"].get("id"), "yes" if i["synthetic"] else "no", i["sha256"]]
                      for i, s in zip(report["inputs"], report["sites"])]), ""]

    out += ["## M5 exit criteria", "", md_table(["Criterion", "Status", "Detail"],
                                                 [[c["name"], c["status"], c["detail"]] for c in report["criteria"]]), ""]

    c = report["combined"]
    tag = " (SYNTHETIC)" if c["synthetic"] else ""
    prefix = "Simulated, not measured: " if c["synthetic"] else ""
    out += [f"## All sites{tag}", ""]
    if c["synthetic"]:
        out += [SYNTHETIC_NOTE, ""]
    out += [f"{prefix}{len(c['sites'])} site(s), {c['units']} unit(s), {num(c['unit_days'], 1)} installed unit-days, reporting "
            f"{pct(c['availability'], 2)} of that time. {c['incidents']} critical alarm(s): {c['by_label']['true']} real hazard, "
            f"{c['by_label']['nuisance']} nuisance, {c['by_label']['test']} test, {c['by_label']['unlabelled']} not labelled. "
            f"{c['drills']} drill(s) logged.", "",
            f"False alarms: {per100_text(c['false_alarms_per_100_unit_days'])}.", ""]
    for title, key in (("Time to acknowledge, drills", "drills"), ("Time to acknowledge, live incidents", "live")):
        block = c["ack_latency"][key]
        out += [f"{title}:", "", _latency_table(block) if block else "No alerts.", ""]

    for s in report["sites"]:
        out += site_section(s)
    return "\n".join(out).rstrip() + "\n"


# --- synthetic demo ----------------------------------------------------------------------------

def _alert(service: str, channel: str, recipient: str, sent: int, ack_after_s: float | None, by: str | None,
           drill: bool, timeouts: int = 0, re_alerts: int = 0, delivery_ms: int = 900) -> dict:
    return {"service": service, "channel": channel, "recipient_masked": recipient, "drill": drill, "sent_at_ms": sent,
            "delivered_at_ms": sent + delivery_ms, "acked_at_ms": None if ack_after_s is None else sent + int(ack_after_s * 1000),
            "acked_by": by if ack_after_s is not None else None, "latency_s": ack_after_s, "timeouts": timeouts,
            "re_alerts": re_alerts, "failed": False}


def _demo_traces(site_dir: Path, scenario: str, unit: str, incident: int, trigger: str) -> list[dict]:
    """A SYNTHETIC trace capture from a simulator scenario, if PyYAML and the engine are available."""
    try:
        sys.path.insert(0, str(HERE.parent / "simulator"))
        import simulate  # noqa: E402
        csv_path, events_path, _ = simulate.write_capture(simulate.Scenario.load(simulate.SCENARIOS / f"{scenario}.yaml"),
                                                          site_dir / "traces", 1)
    except (ImportError, SystemExit, OSError, FileNotFoundError, ValueError):
        return []
    sha = hashlib.sha256(csv_path.read_bytes()).hexdigest()
    rows = max(0, len(csv_path.read_text(encoding="utf-8").splitlines()) - 1)
    return [{"sha256": sha, "unit": unit, "incident": incident, "csv": f"traces/{csv_path.name}",
             "events": f"traces/{events_path.name}", "samples": rows, "period_ms": 2000, "trigger": trigger}]


def write_demo(directory: str | Path) -> list[Path]:
    """Two SYNTHETIC site exports: a hostel with three units and a ward with two. Nothing in them happened."""
    directory = Path(directory)
    t0 = 1_790_000_000_000
    registry = {"sha256": "ab" * 32, "services": {"site": {"mode": "live", "agreement": None},
                                                  "fire": {"mode": "drill", "agreement": None},
                                                  "police": {"mode": "drill", "agreement": None},
                                                  "ambulance": {"mode": "drill", "agreement": None}}}

    a_units = [
        {"id": "a1" * 16, "label": "Room 206", "provisioned_at": t0 - DAY_MS, "fw_versions": ["0.6.0", "0.6.1"],
         "gaps": [{"start_ms": t0 + 9 * DAY_MS, "end_ms": t0 + 9 * DAY_MS + 45 * 60_000, "seconds": 2700}],
         "self_tests": {"pass": 30, "fail": 0}, "faults_raised": 0, "elevated_episodes": 4,
         "fw_updates": [{"ts": (t0 + 12 * DAY_MS) // 1000, "phase": "confirmed", "version": "0.6.1", "from_version": "0.6.0", "reason": ""}]},
        {"id": "a2" * 16, "label": "Kitchen", "provisioned_at": t0 - DAY_MS, "fw_versions": ["0.6.0"], "gaps": [],
         "self_tests": {"pass": 29, "fail": 1}, "faults_raised": 1, "elevated_episodes": 11,
         "fw_updates": [{"ts": (t0 + 12 * DAY_MS) // 1000, "phase": "rolled_back", "version": "0.6.1", "from_version": "0.6.0",
                         "reason": "not confirmed within 60 s: gateway not reachable"}]},
        {"id": "a3" * 16, "label": "Corridor", "provisioned_at": t0 + 2 * DAY_MS, "fw_versions": ["0.6.1"], "gaps": [],
         "self_tests": {"pass": 28, "fail": 0}, "faults_raised": 0, "elevated_episodes": 0, "fw_updates": []},
    ]
    drill_a = t0 + 5 * DAY_MS
    a_doc = {
        "kind": "pilot_site", "synthetic": True, "generated_at": iso_ms(t0 + 30 * DAY_MS),
        "site": {"id": "site-a", "name": "Hostel, Block B (synthetic)"},
        "period": {"from_ms": t0, "to_ms": t0 + 30 * DAY_MS}, "registry": registry, "units": a_units,
        "incidents": [
            {"id": 1, "unit": "a2" * 16, "start_ts": (t0 + 3 * DAY_MS) // 1000, "end_ts": (t0 + 3 * DAY_MS) // 1000 + 60,
             "ts_src": "gateway", "reason": ["gas", "smoke"], "label": "nuisance", "labelled_by": "warden",
             "labelled_at_ms": t0 + 3 * DAY_MS + 25 * 60_000, "late": False, "escalation_state": "finished", "traces": [],
             "alerts": [_alert("site", "sms", "+91••••••4321", t0 + 3 * DAY_MS + 2000, 40, "warden", False)]},
            {"id": 2, "unit": "a1" * 16, "start_ts": (t0 + 9 * DAY_MS) // 1000 + 600, "end_ts": (t0 + 9 * DAY_MS) // 1000 + 660,
             "ts_src": "gateway", "reason": ["flame"], "label": "test", "labelled_by": "warden",
             "labelled_at_ms": t0 + 9 * DAY_MS + 3 * 3_600_000, "late": True, "escalation_state": "withheld", "traces": [],
             "alerts": [_alert("site", "sms", "+91••••••4321", t0 + 9 * DAY_MS + 45 * 60_000, 120, "warden", False)]},
        ],
        "drills": [{"id": 1, "started_at_ms": drill_a, "finished_at_ms": drill_a + 150_000, "by": "admin",
                    "services": ["site", "fire", "police", "ambulance"],
                    "alerts": [_alert("site", "sms", "+91••••••4321", drill_a + 400, 12, "warden", True),
                               _alert("fire", "sms", "+91••••••0042", drill_a + 420, 34, "station-042", True),
                               _alert("police", "sms", "+91••••••0017", drill_a + 410, None, None, True, timeouts=2, re_alerts=1),
                               _alert("ambulance", "voice", "+91••••••0108", drill_a + 450, 52, "ems-north", True, delivery_ms=4200)]}],
        "site_ledger": {"entries": 42, "verify": "PASS  42 entries (synthetic)", "device_id": "5a" * 16},
        "unit_bundles": [{"unit": u["id"], "file": f"units/{u['id'][:8]}.json", "entries": 300, "verify": "PASS  300 entries (synthetic)"}
                         for u in a_units],
        "traces": [],
    }
    for u in a_units:
        u["monitored_s"] = (a_doc["period"]["to_ms"] - max(t0, u["provisioned_at"])) / 1000.0
        u["gap_s"] = sum(g["seconds"] for g in u["gaps"])

    b_units = [
        {"id": "b1" * 16, "label": "Ward 4 bay 1", "provisioned_at": t0, "fw_versions": ["0.6.1"], "gaps": [],
         "self_tests": {"pass": 21, "fail": 0}, "faults_raised": 0, "elevated_episodes": 2, "fw_updates": []},
        {"id": "b2" * 16, "label": "Ward 4 pantry", "provisioned_at": t0, "fw_versions": ["0.6.1"],
         "gaps": [{"start_ms": t0 + 20 * DAY_MS + 3_600_000, "end_ms": None, "seconds": 3000}],
         "self_tests": {"pass": 20, "fail": 0}, "faults_raised": 0, "elevated_episodes": 6, "fw_updates": []},
    ]
    drill_b = t0 + 7 * DAY_MS
    b_doc = {
        "kind": "pilot_site", "synthetic": True, "generated_at": iso_ms(t0 + 21 * DAY_MS),
        "site": {"id": "site-b", "name": "District hospital, Ward 4 (synthetic)"},
        "period": {"from_ms": t0, "to_ms": t0 + 21 * DAY_MS},
        "registry": {**registry, "services": {**registry["services"],
                                              "fire": {"mode": "live", "agreement": {"reference": "FS-17/2026", "date": "2026-11-02"}}}},
        "units": b_units,
        "incidents": [
            {"id": 1, "unit": "b2" * 16, "start_ts": (t0 + 14 * DAY_MS) // 1000, "end_ts": (t0 + 14 * DAY_MS) // 1000 + 420,
             "ts_src": "gateway", "reason": ["smoke", "heat"], "label": "true", "labelled_by": "nurse-in-charge",
             "labelled_at_ms": t0 + 14 * DAY_MS + 50 * 60_000, "late": False, "escalation_state": "finished", "traces": [],
             "alerts": [_alert("site", "sms", "+91••••••7788", t0 + 14 * DAY_MS + 1500, 18, "nurse-in-charge", False),
                        _alert("fire", "voice", "+91••••••0017", t0 + 14 * DAY_MS + 1600, 41, "station-017", False, delivery_ms=5000)]},
            {"id": 2, "unit": "b1" * 16, "start_ts": (t0 + 18 * DAY_MS) // 1000, "end_ts": (t0 + 18 * DAY_MS) // 1000 + 30,
             "ts_src": "uncertain", "reason": ["flame"], "label": None, "labelled_by": None, "labelled_at_ms": None,
             "late": False, "escalation_state": "finished", "traces": [],
             "alerts": [_alert("site", "sms", "+91••••••7788", t0 + 18 * DAY_MS + 1500, None, None, False, timeouts=2, re_alerts=1),
                        _alert("fire", "voice", "+91••••••0017", t0 + 18 * DAY_MS + 1600, 66, "station-017", False)]},
        ],
        "drills": [{"id": 1, "started_at_ms": drill_b, "finished_at_ms": drill_b + 90_000, "by": "admin",
                    "services": ["site", "fire", "police", "ambulance"],
                    "alerts": [_alert("site", "sms", "+91••••••7788", drill_b + 300, 9, "nurse-in-charge", True),
                               _alert("fire", "voice", "+91••••••0017", drill_b + 310, 28, "station-017", True),
                               _alert("police", "sms", "+91••••••0201", drill_b + 320, 47, "ps-201", True),
                               _alert("ambulance", "sms", "+91••••••0108", drill_b + 330, 61, "ems-district", True)]}],
        "site_ledger": {"entries": 27, "verify": "PASS  27 entries (synthetic)", "device_id": "5b" * 16},
        "unit_bundles": [{"unit": u["id"], "file": f"units/{u['id'][:8]}.json", "entries": 180, "verify": "PASS  180 entries (synthetic)"}
                         for u in b_units],
        "traces": [],
    }
    for u in b_units:
        u["monitored_s"] = 21 * 86_400.0
        u["gap_s"] = sum(g["seconds"] for g in u["gaps"])

    paths = []
    for name, doc, scenario, unit, incident, trigger in (("site-a", a_doc, "aerosol-spray", "a2" * 16, 1, "CRITICAL"),
                                                          ("site-b", b_doc, "smouldering-fire", "b2" * 16, 1, "CRITICAL")):
        site_dir = directory / name
        site_dir.mkdir(parents=True, exist_ok=True)
        traces = _demo_traces(site_dir, scenario, unit, incident, trigger)
        doc["traces"] = traces
        for incident_doc in doc["incidents"]:
            if incident_doc["id"] == incident:
                incident_doc["traces"] = [t["sha256"] for t in traces]
        (site_dir / "pilot.json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        paths.append(site_dir)
    return paths
