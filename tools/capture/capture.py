#!/usr/bin/env python3
"""
Record the node's sensor stream to CSV, with timestamped markers.

    pip install -r tools/capture/requirements.txt
    python tools/capture/capture.py --port COM9 --out captures/fan-on-run1 --fan on

Writes <out>.csv (one row per S line, columns from the node's own header, plus
host_s and host_unix_ms) and <out>.events.jsonl (every E line, with the same two
host times). While it runs:

    <Enter>            mark the moment ("mark" event, device timestamp)
    gas in<Enter>      mark with text; the fan notebook pairs "gas in" / "gas out"
    !fan off<Enter>    send any console command to the node
    q<Enter>           stop

Markers are sent to the node and echoed back as events, so they carry the
device clock and line up with samples exactly. host_s is seconds since the
capture started on the host's monotonic clock; host_unix_ms is the host's wall
clock when the line arrived, which is what lets a capture be lined up with the
gateway and the M4 mock endpoints running on the same machine
(tools/validation/alert_latency.py).
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import threading
import time
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxserial import Event, Header, Sample, StreamParser  # noqa: E402

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None


class CaptureWriter:
    """The two capture files, fed one serial line at a time.

    Shared with tools/validation/burnin.py so a burn-in writes exactly the files
    this tool writes. Only the first header becomes the CSV header row: after a
    reboot the node prints the same header again when its stream restarts."""

    def __init__(self, out: str | Path, wall: Callable[[], float] = time.time,
                 monotonic: Callable[[], float] = time.monotonic, append: bool = False):
        out = Path(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        self.csv_path = out.with_suffix(".csv")
        self.events_path = out.with_suffix(".events.jsonl")
        self.parser = StreamParser()
        self.samples = 0
        self.events = 0
        self._wall = wall
        self._monotonic = monotonic
        self._started = monotonic()
        mode = "a" if append else "w"
        have_header = append and self.csv_path.exists() and self.csv_path.stat().st_size > 0
        self._csv_file = open(self.csv_path, mode, newline="", encoding="utf-8")
        self._events_file = open(self.events_path, mode, encoding="utf-8")
        self._writer = csv.writer(self._csv_file)
        self._header_written = have_header
        self._have_header = False

    def host_times(self) -> tuple[float, int]:
        return round(self._monotonic() - self._started, 3), int(self._wall() * 1000)

    def feed(self, line: str) -> Header | Sample | Event | None:
        record = self.parser.feed(line)
        host_s, host_unix_ms = self.host_times()
        if isinstance(record, Header):
            self._have_header = True
            if not self._header_written:
                self._writer.writerow(record.columns + ["host_s", "host_unix_ms"])
                self._header_written = True
        elif isinstance(record, Sample) and self._have_header:
            self._writer.writerow([record.t_ms, *record.values.values(), host_s, host_unix_ms])
            self.samples += 1
            if self.samples % 30 == 0:
                self._csv_file.flush()
        elif isinstance(record, Event):
            self._events_file.write(json.dumps({"t_ms": record.t_ms, "host_s": host_s,
                                                "host_unix_ms": host_unix_ms, **record.data}) + "\n")
            self._events_file.flush()
            self.events += 1
        return record

    def flush(self) -> None:
        self._csv_file.flush()
        self._events_file.flush()

    def close(self) -> None:
        self._csv_file.close()
        self._events_file.close()


def run(args: argparse.Namespace) -> int:
    if serial is None:
        print("pyserial is required: pip install -r tools/capture/requirements.txt", file=sys.stderr)
        return 2

    port = serial.Serial(args.port, args.baud, timeout=0.2)
    capture = CaptureWriter(args.out)
    stop = threading.Event()
    started = time.monotonic()

    def send(command: str) -> None:
        port.write((command.strip() + "\n").encode("ascii", "replace"))

    def reader() -> None:
        buffer = b""
        while not stop.is_set():
            chunk = port.read(4096)
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                line = raw.decode("utf-8", "replace")
                record = capture.feed(line)
                if isinstance(record, Event):
                    print(f"  event {record.data.get('type')}: {json.dumps(record.data)}")
                elif record is None and args.echo and line.strip():
                    print(f"  | {line}")
        capture.close()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    if args.fan is not None:
        send(f"fan {args.fan}")
    send(f"stream on {args.period}")
    print(f"recording to {capture.csv_path} - Enter marks, '!cmd' sends a command, 'q' stops")

    deadline = started + args.duration if args.duration else None
    try:
        while True:
            if deadline and time.monotonic() >= deadline:
                break
            if args.duration:
                time.sleep(0.2)
                continue
            text = sys.stdin.readline()
            if not text or text.strip() == "q":
                break
            text = text.strip()
            send(text[1:] if text.startswith("!") else f"mark {text}".rstrip())
    except KeyboardInterrupt:
        pass
    finally:
        send("stream off")
        time.sleep(0.5)
        stop.set()
        thread.join(timeout=2)
        port.close()

    print(f"{capture.samples} samples, {capture.events} events, {capture.parser.dropped} unparsed records")
    return 0 if capture.samples else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Record the FireX node sensor stream.")
    ap.add_argument("--port", required=True, help="serial port, e.g. COM9 or /dev/ttyUSB0")
    ap.add_argument("--out", required=True, help="output path without extension")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--period", type=int, default=1000, help="stream period in ms (100-60000)")
    ap.add_argument("--fan", help="set the fan first: on, off or 0-100")
    ap.add_argument("--duration", type=float, help="stop after this many seconds (no keyboard input)")
    ap.add_argument("--echo", action="store_true", help="print non-record lines from the node")
    return run(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
