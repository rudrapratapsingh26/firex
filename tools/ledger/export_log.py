#!/usr/bin/env python3
"""
Export the node's ledger (and optionally its captured frames) into a log
bundle the reference verifier checks.

    pip install -r tools/capture/requirements.txt
    python tools/ledger/export_log.py --port COM9 --out captures/m2-demo --media
    npm run verify -- captures/m2-demo.json --media captures/m2-demo-media

Writes <out>.json and, with --media, every JPEG into <out>-media/, each checked
against the SHA-256 the node computed before it is written. The verifier then
checks the chain, every signature against the key the node exported, and that
each media_captured frame hash matches an exported image.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxexport import ExportError, ExportParser  # noqa: E402

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None


def run_command(port, parser: ExportParser, command: str, until, timeout_s: float) -> None:
    port.reset_input_buffer()
    port.write((command + "\n").encode())
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        kind = parser.feed(raw.decode("utf-8", "replace"))
        if kind:
            deadline = time.monotonic() + timeout_s  # progress resets the timeout
        if until(kind):
            return
    raise ExportError(f"timed out waiting for the node to finish `{command}`")


def main() -> int:
    ap = argparse.ArgumentParser(description="Export the FireX node ledger and media for verification.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True, help="output path without extension")
    ap.add_argument("--from-seq", type=int, default=0)
    ap.add_argument("--media", action="store_true", help="also pull every captured JPEG")
    ap.add_argument("--timeout", type=float, default=20.0, help="seconds of silence before giving up")
    args = ap.parse_args()

    if serial is None:
        print("pyserial is required: pip install -r tools/capture/requirements.txt", file=sys.stderr)
        return 2

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    parser = ExportParser()
    try:
        with serial.Serial(args.port, args.baud, timeout=1) as port:
            time.sleep(0.3)
            port.write(b"stream off\n")
            run_command(port, parser, f"ledger export {args.from_seq}", lambda k: k == "X,end", args.timeout)
            print(f"{len(parser.entries)} entries from device {parser.device_id}")

            if args.media:
                run_command(port, parser, "media list", lambda k: k == "M,count", args.timeout)
                media_dir = Path(f"{out}-media")
                media_dir.mkdir(parents=True, exist_ok=True)
                for name in sorted(parser.listed):
                    run_command(port, parser, f"media get {name}", lambda k: k == "M,end", args.timeout)
                    (media_dir / name).write_bytes(parser.files[name].data)
                    print(f"  {name}  {parser.files[name].size} bytes  sha256 {parser.files[name].sha256}")
    except ExportError as exc:
        print(f"export failed: {exc}", file=sys.stderr)
        return 1

    bundle = parser.bundle()
    path = out.with_suffix(".json")
    path.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {path}")
    media_arg = f" --media {out}-media" if args.media else ""
    print(f"verify with:  npm run verify -- {path}{media_arg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
