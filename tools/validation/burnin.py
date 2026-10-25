#!/usr/bin/env python3
"""
The M4 burn-in: hold the node's serial port for 72 hours, record its stream,
inject a flame on a schedule, poll `status`, export the ledger, then report.

    pip install -r tools/capture/requirements.txt
    python tools/validation/burnin.py run --port COM9 --out captures/burnin-2026-10-01 --hours 72 \\
        --inject-every-min 30 --inject-seconds 8 --status-every-min 10 --export-at-end
    python tools/validation/burnin.py report captures/burnin-2026-10-01 \\
        --bundle captures/burnin-2026-10-01-ledger.json --json reports/burnin.json --md reports/burnin.md
    python tools/validation/burnin.py demo --out <dir>/burnin-demo        # SYNTHETIC run and report

The node must run a bench image built with CONFIG_FIREX_BENCH_INJECT: `inject flame
<seconds>` flickers the self-test IR LED in front of the flame sensor, so every
injection goes through the real sensor, scorer, fusion, alarm, camera, ledger and
network. Nothing else on the unit is simulated.

`run` writes, next to <out>:

    <out>.csv, <out>.events.jsonl   the capture, exactly as tools/capture/capture.py writes it
    <out>.status.jsonl              one parsed `status` snapshot per poll
    <out>.schedule.jsonl            every command sent, every refusal, every serial port loss
    <out>-ledger.json               with --export-at-end: the ledger, for the verifier and the report

It survives the node rebooting (the stream is switched back on) and the serial
port disappearing (it reopens the port every few seconds and records the gap).
Ctrl+C stops early; the files written so far are complete.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "capture"))
sys.path.insert(0, str(HERE.parent / "ledger"))
import fxburnin  # noqa: E402
from capture import CaptureWriter  # noqa: E402
from fxexport import ExportError, ExportParser  # noqa: E402
from fxserial import Event, Sample  # noqa: E402

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None


def now_ms() -> int:
    return int(time.time() * 1000)


class Journal:
    """<out>.schedule.jsonl: what the host did and what went wrong on its side."""

    def __init__(self, path: Path, append: bool = False):
        self.handle = open(path, "a" if append else "w", encoding="utf-8")

    def note(self, **fields) -> None:
        self.handle.write(json.dumps({"host_unix_ms": now_ms(), **fields}) + "\n")
        self.handle.flush()

    def close(self) -> None:
        self.handle.close()


def export_ledger(port, path: Path, timeout_s: float = 30.0) -> int:
    """`ledger export 0` into a verifier bundle. Returns the entry count."""
    parser = ExportParser()
    port.reset_input_buffer()
    port.write(b"ledger export 0\n")
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline and not parser.log_done:
        raw = port.readline()
        if raw and parser.feed(raw.decode("utf-8", "replace")):
            deadline = time.monotonic() + timeout_s  # progress resets the timeout
    bundle = parser.bundle()
    path.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
    return len(bundle["log"])


def run(args: argparse.Namespace) -> int:
    if serial is None:
        print("pyserial is required: pip install -r tools/capture/requirements.txt", file=sys.stderr)
        return 2

    out = Path(args.out)
    paths = fxburnin.run_paths(out)
    capture = CaptureWriter(out, append=args.resume)
    journal = Journal(paths["schedule"], append=args.resume)
    status_file = open(paths["status"], "a" if args.resume else "w", encoding="utf-8")
    collector = fxburnin.StatusCollector()
    journal.note(event="run_start", hours=args.hours, inject_every_min=args.inject_every_min,
                 inject_seconds=args.inject_seconds, status_every_min=args.status_every_min, period_ms=args.period)

    started = time.monotonic()
    deadline = started + args.hours * 3600.0
    next_inject = started + args.first_inject_min * 60.0
    next_status = started + 10.0
    stall_s = max(10.0, 5 * args.period / 1000.0)
    port = None
    buffer = b""
    lost_at: float | None = None
    last_sample = started
    last_stream_on = 0.0
    stream_again_at: float | None = None
    injections = 0
    retry_at: float | None = None

    def send(command: str) -> None:
        port.write((command + "\n").encode("ascii", "replace"))
        journal.note(command=command)

    print(f"burn-in for {args.hours} h into {capture.csv_path}; Ctrl+C stops")
    try:
        while time.monotonic() < deadline:
            mono = time.monotonic()
            if port is None:
                try:
                    port = serial.Serial(args.port, args.baud, timeout=0.2)
                except (serial.SerialException, OSError) as exc:
                    if lost_at is None:
                        lost_at = mono
                        journal.note(event="port_lost", error=str(exc))
                        print(f"  serial port unavailable ({exc}); retrying")
                    time.sleep(5)
                    continue
                if lost_at is not None:
                    journal.note(event="port_back", seconds=round(mono - lost_at, 1))
                    print(f"  serial port back after {mono - lost_at:.0f} s")
                    lost_at = None
                buffer = b""
                send(f"stream on {args.period}")
                last_stream_on = last_sample = time.monotonic()

            try:
                chunk = port.read(4096)
                if chunk:
                    buffer += chunk
                    while b"\n" in buffer:
                        raw, buffer = buffer.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace")
                        record = capture.feed(line)
                        if isinstance(record, Sample):
                            last_sample = time.monotonic()
                        elif isinstance(record, Event):
                            kind = record.data.get("type")
                            if kind == "boot":
                                stream_again_at = time.monotonic() + 3.0  # the console starts after boot
                                print(f"  node booted: {record.data.get('reset_reason')}")
                            elif kind in ("fault", "ledger_error") or (kind == "self_test" and record.data.get("result") != "pass"):
                                print(f"  {kind}: {json.dumps(record.data)}")
                        snapshot = collector.feed(line, now_ms())
                        if snapshot:
                            status_file.write(json.dumps(snapshot) + "\n")
                            status_file.flush()
                        reason = fxburnin.refusal(line)
                        if reason is not None:
                            journal.note(response="refused", reason=reason)
                            print(f"  injection refused: {reason}")
                            if "self-test" in reason and retry_at is None:
                                retry_at = time.monotonic() + 60.0  # the schedule met the daily self-test

                mono = time.monotonic()
                if stream_again_at is not None and mono >= stream_again_at:
                    send(f"stream on {args.period}")
                    last_stream_on = mono
                    stream_again_at = None
                elif mono - last_sample > stall_s and mono - last_stream_on > 15.0:
                    send(f"stream on {args.period}")
                    last_stream_on = mono
                if retry_at is not None and mono >= retry_at:
                    retry_at = None
                    journal.note(event="inject_retry")
                    send(f"inject flame {args.inject_seconds}")
                    injections += 1
                if mono >= next_inject:
                    send(f"inject flame {args.inject_seconds}")
                    injections += 1
                    next_inject += args.inject_every_min * 60.0
                    while next_inject <= mono:  # slots lost while the port was gone are not made up
                        journal.note(event="inject_skipped", reason="serial port was unavailable")
                        next_inject += args.inject_every_min * 60.0
                if mono >= next_status:
                    send("status")
                    next_status += args.status_every_min * 60.0
                    while next_status <= mono:
                        next_status += args.status_every_min * 60.0
            except (serial.SerialException, OSError) as exc:
                journal.note(event="port_lost", error=str(exc))
                print(f"  serial port lost ({exc}); reopening")
                lost_at = time.monotonic()
                try:
                    port.close()
                except Exception:  # noqa: BLE001 - the port is already gone
                    pass
                port = None
                time.sleep(2)
    except KeyboardInterrupt:
        journal.note(event="stopped_early")
        print("  stopped by operator")

    try:
        if port is not None:
            send("stream off")
            time.sleep(1.0)
            if args.export_at_end:
                count = export_ledger(port, paths["ledger"])
                journal.note(event="ledger_exported", entries=count)
                print(f"  exported {count} ledger entries to {paths['ledger']}")
    except (ExportError, serial.SerialException, OSError) as exc:
        journal.note(event="export_failed", error=str(exc))
        print(f"  ledger export failed: {exc}; run tools/ledger/export_log.py by hand", file=sys.stderr)
    finally:
        journal.note(event="run_end", injections=injections, samples=capture.samples, events=capture.events)
        capture.close()
        status_file.close()
        journal.close()
        if port is not None:
            port.close()

    print(f"{capture.samples} samples, {capture.events} events, {injections} injections sent")
    bundle = f" --bundle {paths['ledger']}" if paths["ledger"].exists() else ""
    print(f"report with:  python tools/validation/burnin.py report {out}{bundle}")
    return 0


def report(args: argparse.Namespace) -> int:
    bundle = args.bundle
    if bundle is None and fxburnin.run_paths(args.out)["ledger"].exists():
        bundle = fxburnin.run_paths(args.out)["ledger"]
        print(f"using the ledger export {bundle}")
    result = fxburnin.analyse(args.out, bundle_path=bundle, tta_limit_s=args.tta_limit_s,
                              heap_slope_min=args.heap_slope_min, heap_floor=args.heap_floor, max_gap_s=args.max_gap_s)
    return emit(result, args.json, args.md)


def emit(result: dict, json_path: str | None, md_path: str | None) -> int:
    tag = "SYNTHETIC " if result["synthetic"] else ""
    print(f"{tag}{'PASS' if result['pass'] else 'FAIL'} over {result['hours']:.1f} h")
    for c in result["criteria"]:
        print(f"  {'ok  ' if c['ok'] else 'FAIL'}  {c['name']}: {c['detail']}")
    for path, text in ((json_path, json.dumps(result, indent=2) + "\n"), (md_path, fxburnin.markdown(result))):
        if path:
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            Path(path).write_text(text, encoding="utf-8")
            print(f"wrote {path}")
    return 0 if result["pass"] else 1


def demo(args: argparse.Namespace) -> int:
    bundle = fxburnin.demo_run(args.out, hours=args.hours)
    result = fxburnin.analyse(args.out, bundle_path=bundle, synthetic=True)
    emit(result, args.json or f"{args.out}-report.json", args.md or f"{args.out}-report.md")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="FireX M4 burn-in: run it, then report on it.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="hold the serial port, inject on a schedule, poll status")
    r.add_argument("--port", required=True, help="serial port, e.g. COM9 or /dev/ttyUSB0")
    r.add_argument("--out", required=True, help="output path without extension")
    r.add_argument("--baud", type=int, default=115200)
    r.add_argument("--hours", type=float, default=72.0)
    r.add_argument("--inject-every-min", type=float, default=30.0)
    r.add_argument("--inject-seconds", type=int, default=8, help="3-30; the firmware refuses anything else")
    r.add_argument("--first-inject-min", type=float, default=5.0, help="minutes before the first injection")
    r.add_argument("--status-every-min", type=float, default=10.0)
    r.add_argument("--period", type=int, default=1000, help="stream period in ms")
    r.add_argument("--export-at-end", action="store_true", help="export the ledger into <out>-ledger.json")
    r.add_argument("--resume", action="store_true", help="append to existing files instead of starting them again")
    r.set_defaults(fn=run)

    p = sub.add_parser("report", help="judge a finished run")
    p.add_argument("out", help="the run's output path without extension")
    p.add_argument("--bundle", help="the ledger export (default: <out>-ledger.json if it exists)")
    p.add_argument("--tta-limit-s", type=float, default=15.0, help="injection start to CRITICAL limit")
    p.add_argument("--heap-slope-min", type=float, default=-2000.0, help="lowest acceptable free-heap slope, bytes/h")
    p.add_argument("--heap-floor", type=int, default=30000, help="free heap must stay above this")
    p.add_argument("--max-gap-s", type=float, default=60.0, help="longer stream silences count as gaps")
    p.add_argument("--json", help="write the JSON report here (for tools/validation/report.py)")
    p.add_argument("--md", help="write the Markdown report here")
    p.set_defaults(fn=report)

    d = sub.add_parser("demo", help="write a SYNTHETIC run and report on it")
    d.add_argument("--out", required=True, help="output path without extension")
    d.add_argument("--hours", type=float, default=6.0)
    d.add_argument("--json")
    d.add_argument("--md")
    d.set_defaults(fn=demo)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
