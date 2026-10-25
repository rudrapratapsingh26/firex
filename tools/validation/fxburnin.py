"""
Analysis of an M4 burn-in: a node left running for 72 hours while the host
injects a flame every half hour, then asked whether anything went wrong.

    python tools/validation/burnin.py report captures/burnin-2026-10-01 --bundle captures/burnin-2026-10-01-ledger.json

Input is what tools/validation/burnin.py writes next to <out>:

    <out>.csv               the capture stream (tools/capture format, with host_unix_ms)
    <out>.events.jsonl      every E line: ledger echoes, inject start/end, marks
    <out>.status.jsonl      a parsed `status` snapshot every few minutes
    <out>.schedule.jsonl    every command the host sent, refusals, serial port losses
    <out>-ledger.json       optional: the ledger exported at the end (tools/ledger format)

The roadmap's M4 criterion is "zero FAULT entries and zero watchdog resets". This
module checks that and the things that would hide a failure from it: an injection
that did not produce CRITICAL, a CRITICAL nobody injected, a failed self-test, a
ledger write error or dropped event, entries overwritten before the gateway had
them, a fault flag that never reaches the ledger (the buzzer), and free heap that
keeps falling. Serial output can drop lines; the exported ledger cannot, so when
a bundle is given its counts win and the two are compared.

Standard library only. demo_run() writes a SYNTHETIC run; nothing derived from it
is a measurement.
"""
from __future__ import annotations

import csv
import json
import random
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxstats  # noqa: E402
from fxlatency import boot_segments, csv_host_times, load_jsonl, sha256_file  # noqa: E402

BAD_RESETS = ("watchdog", "panic", "brownout")
EXPECTED_BOOTS = ("power_on", "external", "software")
RESPONSE_WINDOW_MS = 10_000
RETRY_WINDOW_MS = 180_000  # a self-test collision's retry must follow within this


# --- files ------------------------------------------------------------------

def run_paths(out: str | Path) -> dict[str, Path]:
    """The files of one run. Named the way tools/capture/capture.py names a capture."""
    out = Path(out)
    return {
        "csv": out.with_suffix(".csv"),
        "events": out.with_suffix(".events.jsonl"),
        "status": out.with_suffix(".status.jsonl"),
        "schedule": out.with_suffix(".schedule.jsonl"),
        "ledger": Path(f"{out}-ledger.json"),
    }


# --- `status` output ------------------------------------------------------------

STATUS_HEAD = re.compile(r"FireX node (\S+), boot (\d+) \(([^)]*)\), uptime (\d+) s, free heap (\d+)")
STATUS_FAULTS = re.compile(r"faults: sensor (\w+), ledger (\w+), self-test (\w+), buzzer (\w+)")
STATUS_LEDGER = re.compile(r"next seq (\d+), appended this boot (\d+), failures (\d+)")
STATUS_QUEUE = re.compile(r"queue dropped (\d+)")


@dataclass
class StatusCollector:
    """Turns the lines of one `status` reply into one snapshot. The reply's lines may
    be interleaved with stream records; anything that matches none of the patterns is
    ignored. A snapshot is complete at the `queue dropped` line, the last one."""

    current: dict | None = None

    def feed(self, line: str, host_unix_ms: int) -> dict | None:
        text = line.strip()
        head = STATUS_HEAD.search(text)
        if head:
            self.current = {
                "host_unix_ms": host_unix_ms, "fw_version": head.group(1), "boot": int(head.group(2)),
                "reset_reason": head.group(3), "uptime_s": int(head.group(4)), "free_heap": int(head.group(5)),
            }
            return None
        if self.current is None:
            return None
        faults = STATUS_FAULTS.search(text)
        if faults:
            names = ("sensor", "ledger", "selftest", "buzzer")
            self.current["faults"] = {n: v.upper() == "YES" for n, v in zip(names, faults.groups())}
            return None
        ledger = STATUS_LEDGER.search(text)
        if ledger:
            self.current.update(next_seq=int(ledger.group(1)), appended=int(ledger.group(2)),
                                failures=int(ledger.group(3)))
            return None
        queue = STATUS_QUEUE.search(text)
        if queue:
            snapshot, self.current = self.current, None
            snapshot["queue_dropped"] = int(queue.group(1))
            return snapshot
        return None


INJECT_REFUSED = re.compile(r"inject refused:\s*(.*)$")


def refusal(line: str) -> str | None:
    match = INJECT_REFUSED.search(line.strip())
    return match.group(1).strip() if match else None


# --- the analysis ------------------------------------------------------------------

def _flat_bundle_entries(bundle: dict) -> list[dict]:
    flat = []
    for entry in bundle.get("log", []):
        payload = entry.get("payload") or {}
        flat.append({**payload, "type": entry.get("type"), "seq": entry.get("seq")})
    return flat


@dataclass
class Counts:
    faults_raised: int = 0
    fault_sources: dict = field(default_factory=dict)
    boots: int = 0
    boot_reasons: dict = field(default_factory=dict)
    self_pass: int = 0
    self_fail: int = 0
    overwrite_mirrored: int = 0
    overwrite_unmirrored: int = 0

    @staticmethod
    def of(entries: list[dict]) -> "Counts":
        c = Counts()
        for e in entries:
            kind = e.get("type")
            if kind == "fault" and e.get("state") == "raised":
                c.faults_raised += 1
                source = str(e.get("source", "?"))
                c.fault_sources[source] = c.fault_sources.get(source, 0) + 1
            elif kind == "boot":
                c.boots += 1
                reason = str(e.get("reset_reason", "?"))
                c.boot_reasons[reason] = c.boot_reasons.get(reason, 0) + 1
            elif kind == "self_test":
                if e.get("result") == "pass":
                    c.self_pass += 1
                else:
                    c.self_fail += 1
            elif kind == "log_overwrite":
                if e.get("mirrored") is True:
                    c.overwrite_mirrored += 1
                else:
                    c.overwrite_unmirrored += 1
        return c


def _match_injections(schedule: list[dict], events: list[dict], limit_ms: int) -> tuple[dict, list[dict], list[dict]]:
    """Pair each inject command with its start echo (or its refusal), then with the
    first CRITICAL after it. Returns the injection summary, per-injection rows, and the
    CRITICAL transitions no injection explains."""
    sends = [s for s in schedule if str(s.get("command", "")).startswith("inject")]
    refusals = [s for s in schedule if s.get("response") == "refused"]
    starts = [e for e in events if e.get("type") == "inject" and e.get("phase") == "start"]
    ordered = sorted(events, key=lambda e: e.get("host_unix_ms", 0))
    points = [(e.get("host_unix_ms", 0), e.get("t_ms", 0), e.get("type") == "boot") for e in ordered]
    segment_of = {id(e): s for e, s in zip(ordered, boot_segments(points))}
    criticals = [e for e in ordered if e.get("type") == "state_change" and e.get("to") == "CRITICAL"]

    used_starts: set[int] = set()
    used_refusals: set[int] = set()
    used_criticals: set[int] = set()
    rows, missed, tta = [], [], []

    for send in sends:
        sent = int(send.get("host_unix_ms", 0))
        start = next((e for e in starts if id(e) not in used_starts
                      and sent - 1000 <= e.get("host_unix_ms", 0) <= sent + RESPONSE_WINDOW_MS), None)
        row = {"host_unix_ms": sent, "command": send.get("command"), "started": start is not None,
               "detected": False, "tta_s": None, "reason": None}
        if start is None:
            refused = next((r for r in refusals if id(r) not in used_refusals
                            and sent - 1000 <= r.get("host_unix_ms", 0) <= sent + RESPONSE_WINDOW_MS), None)
            if refused is not None:
                used_refusals.add(id(refused))
                row["reason"] = f"refused: {refused.get('reason', '')}".strip()
            else:
                row["reason"] = "no response from the node"
            rows.append(row)
            missed.append({"host_unix_ms": sent, "reason": row["reason"]})
            continue
        used_starts.add(id(start))
        begin = int(start.get("host_unix_ms", 0))
        crit = next((c for c in criticals if id(c) not in used_criticals
                     and begin - 1000 <= c.get("host_unix_ms", 0) <= begin + limit_ms), None)
        if crit is None:
            row["reason"] = f"no CRITICAL within {limit_ms / 1000:.0f} s"
            missed.append({"host_unix_ms": sent, "reason": row["reason"]})
        else:
            used_criticals.add(id(crit))
            same_boot = segment_of[id(crit)] == segment_of[id(start)] and crit["t_ms"] >= start["t_ms"]
            seconds = ((crit["t_ms"] - start["t_ms"]) if same_boot else (crit["host_unix_ms"] - begin)) / 1000.0
            row.update(detected=True, tta_s=seconds)
            tta.append(seconds)
        rows.append(row)

    # An injection refused because the daily self-test held the LED is the schedule
    # colliding with the unit, not the unit failing: the runner retries it a minute
    # later, and the retry is what counts.
    retries = [int(s.get("host_unix_ms", 0)) for s in schedule if s.get("event") == "inject_retry"]
    deferred = []
    for row in rows:
        if (not row["started"] and "self-test" in (row["reason"] or "")
                and any(0 < t - row["host_unix_ms"] <= RETRY_WINDOW_MS for t in retries)):
            row["deferred"] = True
            deferred.append({"host_unix_ms": row["host_unix_ms"], "reason": row["reason"]})
    deferred_at = {d["host_unix_ms"] for d in deferred}
    missed = [m for m in missed if m["host_unix_ms"] not in deferred_at]
    counted = len(sends) - len(deferred)

    unexpected = [{"host_unix_ms": c.get("host_unix_ms"), "seq": c.get("seq"), "reason": c.get("reason")}
                  for c in criticals if id(c) not in used_criticals]
    summary = {"sent": counted, "detected": fxstats.rate(len(tta), counted),
               "tta_s": fxstats.summarize(tta), "missed": missed, "deferred": deferred}
    return summary, rows, unexpected


def _heap(snapshots: list[dict], min_uptime_s: int = 600) -> dict:
    heaps = [s["free_heap"] for s in snapshots if "free_heap" in s]
    result = {"first": heaps[0] if heaps else None, "last": heaps[-1] if heaps else None,
              "min": min(heaps) if heaps else None, "slope_bytes_per_h": None, "slope_basis": None}
    by_boot: dict[int, list[dict]] = {}
    segment, last_uptime = 0, None
    for s in snapshots:
        if "free_heap" not in s:
            continue
        if last_uptime is not None and s["uptime_s"] < last_uptime:
            segment += 1
        last_uptime = s["uptime_s"]
        if s["uptime_s"] >= min_uptime_s:
            by_boot.setdefault(segment, []).append(s)
    if not by_boot:
        result["slope_basis"] = f"no status snapshot after {min_uptime_s} s of uptime"
        return result
    longest = max(by_boot.values(), key=lambda pts: pts[-1]["uptime_s"] - pts[0]["uptime_s"])
    span_h = (longest[-1]["uptime_s"] - longest[0]["uptime_s"]) / 3600.0
    if len(longest) < 3 or span_h < 1.0:
        result["slope_basis"] = f"{len(longest)} snapshots over {span_h:.1f} h: too few to fit"
        return result
    xs = [s["uptime_s"] / 3600.0 for s in longest]
    ys = [float(s["free_heap"]) for s in longest]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    result["slope_bytes_per_h"] = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    result["slope_basis"] = f"least squares over {len(longest)} snapshots, {span_h:.1f} h of one boot, uptime >= {min_uptime_s} s"
    return result


def _gaps(times: list[int], min_gap_s: float) -> list[dict]:
    gaps = []
    for a, b in zip(times, times[1:]):
        if (b - a) / 1000.0 > min_gap_s:
            gaps.append({"from_host_unix_ms": a, "seconds": round((b - a) / 1000.0, 1)})
    return gaps


def _cross_check(serial: list[dict], bundle: list[dict]) -> tuple[list[dict], dict]:
    """Merge serial echoes with the exported ledger over the run's sequence range.
    The bundle wins where both hold an entry; serial fills in anything rotated out
    before the export."""
    serial_by_seq = {e["seq"]: e for e in serial if isinstance(e.get("seq"), int)}
    notes: list[str] = []
    if not serial_by_seq:
        notes.append("no ledger echoes carried a seq: every bundle entry is counted")
        first = min((e["seq"] for e in bundle), default=0)
    else:
        first = min(serial_by_seq)
    bundle_by_seq = {e["seq"]: e for e in bundle if isinstance(e.get("seq"), int) and e["seq"] >= first}
    if bundle_by_seq and min(bundle_by_seq) > first:
        notes.append(f"the export starts at seq {min(bundle_by_seq)}, after the run's first entry {first}: "
                     "earlier entries rotated out and are counted from serial only")
    bundle_first = min(bundle_by_seq) if bundle_by_seq else None
    missing_from_serial = [s for s in bundle_by_seq if s not in serial_by_seq]
    missing_from_bundle = [s for s in serial_by_seq if bundle_first is not None and s >= bundle_first
                           and s not in bundle_by_seq]
    mismatch = [s for s in bundle_by_seq if s in serial_by_seq and serial_by_seq[s].get("type") != bundle_by_seq[s].get("type")]
    merged = {**{s: e for s, e in serial_by_seq.items()}, **bundle_by_seq}
    if missing_from_bundle:
        notes.append(f"{len(missing_from_bundle)} entries echoed on serial are not in the export: investigate")
    check = {
        "bundle_entries": len(bundle_by_seq),
        "serial_entries": len(serial_by_seq),
        "missing_from_serial": len(missing_from_serial),
        "missing_from_bundle": len(missing_from_bundle),
        "type_mismatch": len(mismatch),
        "examples": {"missing_from_serial": sorted(missing_from_serial)[:10],
                     "missing_from_bundle": sorted(missing_from_bundle)[:10], "type_mismatch": sorted(mismatch)[:10]},
        "notes": notes,
    }
    return [merged[s] for s in sorted(merged)], check


def analyse(out: str | Path, bundle_path: str | Path | None = None, tta_limit_s: float = 15.0,
            heap_slope_min: float = -2000.0, heap_floor: int = 30000, max_gap_s: float = 60.0,
            synthetic: bool = False) -> dict:
    paths = run_paths(out)
    events = load_jsonl(paths["events"])
    status = load_jsonl(paths["status"]) if paths["status"].exists() else []
    schedule = load_jsonl(paths["schedule"]) if paths["schedule"].exists() else []
    samples = csv_host_times(paths["csv"]) if paths["csv"].exists() else []
    synthetic = synthetic or any(s.get("synthetic") for s in schedule)

    inputs = [{"path": str(p), "sha256": sha256_file(p)} for key, p in paths.items()
              if key != "ledger" and p.exists()]
    ledger_echoes = [e for e in events if isinstance(e.get("seq"), int)]
    cross = None
    counted = ledger_echoes
    if bundle_path:
        bundle = json.loads(Path(bundle_path).read_text(encoding="utf-8"))
        inputs.append({"path": str(bundle_path), "sha256": sha256_file(bundle_path)})
        counted, cross = _cross_check(ledger_echoes, _flat_bundle_entries(bundle))
    counts = Counts.of(counted)

    host_times = sorted([e.get("host_unix_ms", 0) for e in events] + [h for h, _ in samples]
                        + [s.get("host_unix_ms", 0) for s in status + schedule])
    host_times = [t for t in host_times if t]
    hours = (host_times[-1] - host_times[0]) / 3_600_000.0 if len(host_times) > 1 else 0.0

    injections, injection_rows, unexpected = _match_injections(schedule, events, int(tta_limit_s * 1000))
    ledger_errors = sum(1 for e in events if e.get("type") == "ledger_error")
    heap = _heap(status)
    sample_times = sorted(h for h, _ in samples)
    gaps = _gaps(sample_times, max_gap_s)
    port_losses = [s for s in schedule if s.get("event") == "port_lost"]
    flagged = [s for s in status if any((s.get("faults") or {}).values())]
    dropped = max((s.get("queue_dropped", 0) for s in status), default=0)
    failures = max((s.get("failures", 0) for s in status), default=0)
    bad_resets = {r: counts.boot_reasons.get(r, 0) for r in BAD_RESETS}
    other_resets = sum(n for r, n in counts.boot_reasons.items() if r not in BAD_RESETS + EXPECTED_BOOTS)

    def crit(name: str, ok: bool, detail: str) -> dict:
        return {"name": name, "ok": bool(ok), "detail": detail}

    sources = ", ".join(f"{k} {v}" for k, v in sorted(counts.fault_sources.items())) or "none"
    reasons = ", ".join(f"{k} {v}" for k, v in sorted(counts.boot_reasons.items())) or "no boot entries"
    criteria = [
        crit("no fault raised", counts.faults_raised == 0, f"{counts.faults_raised} raised ({sources})"),
        crit("no watchdog, panic or brownout reset", sum(bad_resets.values()) == 0, f"boots: {reasons}"),
        crit("every injection reached CRITICAL in time", injections["sent"] > 0 and not injections["missed"],
             f"{fxstats.rate_text(injections['detected'])} within {tta_limit_s:.0f} s"
             + (f"; missed: {', '.join(m['reason'] for m in injections['missed'][:5])}" if injections["missed"] else "")),
        crit("no CRITICAL without an injection", not unexpected, f"{len(unexpected)} unexplained"),
        crit("every self-test passed", counts.self_fail == 0,
             f"{counts.self_pass} passed, {counts.self_fail} failed"),
        crit("no ledger write error or dropped event", ledger_errors == 0 and dropped == 0 and failures == 0,
             f"{ledger_errors} ledger_error echoes, {dropped} events dropped, {failures} append failures (status)"),
        crit("nothing overwritten before the gateway had it", counts.overwrite_unmirrored == 0,
             f"{counts.overwrite_unmirrored} unmirrored, {counts.overwrite_mirrored} mirrored log_overwrite entries"),
        crit("status never showed a fault flag", not flagged,
             f"{len(flagged)} of {len(status)} snapshots flagged" if status else "no status snapshots"),
        crit("free heap steady", heap["slope_bytes_per_h"] is not None and heap["slope_bytes_per_h"] >= heap_slope_min
             and heap["min"] is not None and heap["min"] > heap_floor,
             "not measured: " + str(heap["slope_basis"]) if heap["slope_bytes_per_h"] is None else
             f"slope {heap['slope_bytes_per_h']:.0f} bytes/h (bound {heap_slope_min:.0f}), min {heap['min']} (floor {heap_floor})"),
        crit("record complete", bool(bundle_path) or (not gaps and not port_losses),
             f"{len(gaps)} stream gaps over {max_gap_s:.0f} s, {len(port_losses)} serial port losses"
             + ("; ledger export supplied" if bundle_path else "")),
    ]

    return {
        "kind": "burnin",
        "synthetic": synthetic,
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "inputs": inputs,
        "hours": round(hours, 2),
        "injections": injections,
        "injection_log": injection_rows,
        "unexpected_critical": unexpected,
        "faults_raised": counts.faults_raised,
        "fault_sources": counts.fault_sources,
        "resets": {**bad_resets, "other": other_resets},
        "boot_reasons": counts.boot_reasons,
        "boots": counts.boots,
        "self_tests": {"pass": counts.self_pass, "fail": counts.self_fail},
        "ledger_errors": ledger_errors,
        "events_dropped": dropped,
        "log_overwrite": {"mirrored": counts.overwrite_mirrored, "unmirrored": counts.overwrite_unmirrored},
        "heap": heap,
        "stream_gaps": gaps,
        "port_losses": len(port_losses),
        "cross_check": cross,
        "criteria": criteria,
        "pass": all(c["ok"] for c in criteria),
    }


def markdown(report: dict) -> str:
    lines = ["# FireX burn-in report", ""]
    if report["synthetic"]:
        lines += ["> **SYNTHETIC DATA.** Generated by the tool's demo, not recorded from a node. "
                  "Nothing here is a measurement.", ""]
    verdict = "PASS" if report["pass"] else "FAIL"
    lines += [f"**{verdict}** over {report['hours']:.1f} h, generated {report['generated_at']}.", ""]
    lines += [fxstats.md_table(["Criterion", "Result", "Detail"],
                               [[c["name"], "ok" if c["ok"] else "**FAIL**", c["detail"]] for c in report["criteria"]]), ""]
    inj = report["injections"]
    lines += ["## Injections", "",
              f"{inj['sent']} sent; detected {fxstats.rate_text(inj['detected'])}; "
              f"time from injection start to CRITICAL {fxstats.summary_text(inj['tta_s'], 1, 's')}.", ""]
    if inj["missed"]:
        lines += [fxstats.md_table(["Host time (UTC)", "Why"],
                                   [[_utc(m["host_unix_ms"]), m["reason"]] for m in inj["missed"]]), ""]
    heap = report["heap"]
    lines += ["## Node health", "",
              fxstats.md_table(["Measure", "Value"], [
                  ["boots", f"{report['boots']} ({', '.join(f'{k} {v}' for k, v in sorted(report['boot_reasons'].items())) or 'none'})"],
                  ["faults raised", f"{report['faults_raised']} {report['fault_sources'] or ''}".strip()],
                  ["self-tests", f"{report['self_tests']['pass']} passed, {report['self_tests']['fail']} failed"],
                  ["ledger errors / dropped events", f"{report['ledger_errors']} / {report['events_dropped']}"],
                  ["log_overwrite mirrored / unmirrored", f"{report['log_overwrite']['mirrored']} / {report['log_overwrite']['unmirrored']}"],
                  ["free heap first / last / min", f"{heap['first']} / {heap['last']} / {heap['min']}"],
                  ["free heap slope", "-" if heap["slope_bytes_per_h"] is None else f"{heap['slope_bytes_per_h']:.0f} bytes/h ({heap['slope_basis']})"],
                  ["stream gaps / port losses", f"{len(report['stream_gaps'])} / {report['port_losses']}"],
              ]), ""]
    cross = report.get("cross_check")
    if cross:
        lines += ["## Serial against the exported ledger", "",
                  f"{cross['bundle_entries']} entries in the export, {cross['serial_entries']} echoed on serial; "
                  f"{cross['missing_from_serial']} missing from serial, {cross['missing_from_bundle']} missing from the export, "
                  f"{cross['type_mismatch']} with different types.", ""]
        lines += [f"- {note}" for note in cross["notes"]] + ([""] if cross["notes"] else [])
    lines += ["## Inputs", ""] + [f"- `{i['path']}` sha256 `{i['sha256'][:16]}…`" for i in report["inputs"]] + [""]
    return "\n".join(lines)


def _utc(ms: int | None) -> str:
    if not ms:
        return "-"
    return datetime.fromtimestamp(ms / 1000.0, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


# --- synthetic demo run (NOT measurements) -----------------------------------------

class SyntheticRun:
    """Writes the files of a burn-in the way burnin.py would, for tests and the demo.
    Device time restarts at zero on reboot(); host time never goes back."""

    def __init__(self, out: str | Path, start_unix_ms: int = 1_790_000_000_000, period_ms: int = 1000):
        self.paths = run_paths(out)
        self.paths["csv"].parent.mkdir(parents=True, exist_ok=True)
        self.host = start_unix_ms
        self.boot_host = start_unix_ms
        self.seq = 100
        self.boot_count = 7
        self.period_ms = period_ms
        self.heap = 180_000
        self.csv = open(self.paths["csv"], "w", newline="", encoding="utf-8")
        self.writer = csv.writer(self.csv)
        self.writer.writerow(["t_ms", "gas", "gas_ok", "flame", "flame_ok", "alarm", "host_s", "host_unix_ms"])
        self.events = open(self.paths["events"], "w", encoding="utf-8")
        self.status = open(self.paths["status"], "w", encoding="utf-8")
        self.schedule = open(self.paths["schedule"], "w", encoding="utf-8")
        self.schedule.write(json.dumps({"host_unix_ms": self.host, "synthetic": True, "note": "SYNTHETIC run"}) + "\n")
        self.ledger: list[dict] = []
        self.flame = 0.0
        self.alarm = "NORMAL"

    @property
    def t_ms(self) -> int:
        return self.host - self.boot_host

    def event(self, kind: str, ledger: bool = True, echo: bool = True, **fields) -> dict:
        """A ledger entry (ledger=True) or a serial-only event. echo=False writes the entry
        to the ledger but loses its serial line, as a dropped USB connection would."""
        record = {"t_ms": self.t_ms, "host_s": round((self.host - self.boot_host) / 1000.0, 3),
                  "host_unix_ms": self.host, "type": kind}
        if ledger:
            record["seq"] = self.seq
            self.ledger.append({"seq": self.seq, "type": kind, "payload": dict(fields)})
            self.seq += 1
        record.update(fields)
        if echo:
            self.events.write(json.dumps(record) + "\n")
        return record

    def advance(self, ms: int, stream: bool = True) -> None:
        end = self.host + ms
        while self.host + self.period_ms <= end:
            self.host += self.period_ms
            if stream:
                self.writer.writerow([self.t_ms, "0.050", 1, f"{self.flame:.3f}", 1, self.alarm,
                                      round(self.t_ms / 1000.0, 3), self.host])
        self.host = end

    def boot(self, reason: str = "power_on") -> None:
        self.boot_host = self.host - 300
        self.boot_count += 1
        self.event("boot", fw_version="0.5.0+inject", reset_reason=reason, boot_count=self.boot_count)

    def snapshot(self, faults: dict | None = None, dropped: int = 0) -> None:
        self.status.write(json.dumps({
            "host_unix_ms": self.host, "fw_version": "0.5.0+inject", "boot": self.boot_count, "reset_reason": "power_on",
            "uptime_s": self.t_ms // 1000, "free_heap": self.heap,
            "faults": faults or {"sensor": False, "ledger": False, "selftest": False, "buzzer": False},
            "next_seq": self.seq, "appended": 0, "failures": 0, "queue_dropped": dropped,
        }) + "\n")

    def command(self, text: str) -> None:
        self.schedule.write(json.dumps({"host_unix_ms": self.host, "command": text}) + "\n")

    def inject(self, seconds: int = 8, critical_after_ms: int | None = 4500, refuse: str | None = None) -> None:
        self.command(f"inject flame {seconds}")
        self.advance(150)
        if refuse:
            self.schedule.write(json.dumps({"host_unix_ms": self.host, "response": "refused", "reason": refuse}) + "\n")
            return
        self.event("inject", ledger=False, source="flame", seconds=seconds, phase="start")
        if critical_after_ms is not None:
            self.flame = 0.95
            self.advance(critical_after_ms - 1500)
            self.event("threshold_cross", modality="flame", score=95, direction="up")
            self.advance(1500)
            self.alarm = "CRITICAL"
            self.event("state_change", **{"from": "NORMAL", "to": "CRITICAL", "reason": ["flame"], "scores": {"flame": 95}})
            self.event("media_captured", trigger="CRITICAL", frames=[], clip=None, codec="jpeg", resolution="none")
            self.advance(seconds * 1000 - critical_after_ms)
        else:
            self.advance(seconds * 1000)
        self.event("inject", ledger=False, source="flame", seconds=seconds, phase="end")
        self.flame = 0.0
        if critical_after_ms is not None:
            self.advance(2500)
            self.event("threshold_cross", modality="flame", score=30, direction="down")
            self.alarm = "NORMAL"
            self.event("state_change", **{"from": "CRITICAL", "to": "NORMAL", "reason": [], "scores": {"flame": 30}})

    def close(self) -> None:
        for handle in (self.csv, self.events, self.status, self.schedule):
            handle.close()
        bundle = {"device_id": "ab" * 16, "public_key_pem": "SYNTHETIC", "media": [],
                  "log": [{"seq": e["seq"], "ts": 0, "ts_src": "uncertain", "device_id": "ab" * 16, "type": e["type"],
                           "payload": e["payload"], "fw_hash": "00" * 32, "prev_hash": "00" * 32,
                           "hash": "00" * 32, "sig": "00" * 64} for e in self.ledger]}
        self.paths["ledger"].write_text(json.dumps(bundle), encoding="utf-8")


def demo_run(out: str | Path, hours: float = 6.0, seed: int = 11) -> Path:
    """SYNTHETIC: a compressed burn-in with an injection every 30 minutes, a status
    snapshot every 10 minutes, heap wandering around a flat line, and one injection
    refused because a self-test was running. Returns the ledger bundle path."""
    rng = random.Random(seed)
    run = SyntheticRun(out)
    run.boot("power_on")
    run.advance(200_000)
    run.event("self_test", result="pass", checks={"heater": True, "buzzer": True, "camera": True, "ledger": True})
    minutes = int(hours * 60)
    for minute in range(minutes):
        if minute % 10 == 0:
            run.heap = 180_000 + rng.randint(-1500, 1500)
            run.snapshot()
        if minute % 30 == 15:
            if minute == 135:
                run.event("self_test", result="pass", checks={"heater": True, "buzzer": True})
                run.inject(refuse="a self-test is running")
            else:
                run.inject(critical_after_ms=4000 + rng.randint(0, 1500))
        run.advance(60_000 - (run.host - run.boot_host) % 60_000 or 60_000)
    run.close()
    return run.paths["ledger"]
