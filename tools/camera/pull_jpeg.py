#!/usr/bin/env python3
"""
Pull a JPEG from the ESP32-CAM bring-up image over serial.

    pip install -r tools/capture/requirements.txt
    python tools/camera/pull_jpeg.py --port COM10                 # VGA, quality 12
    python tools/camera/pull_jpeg.py --port COM10 --size svga --quality 10

Saves captures/camera/snap-<UTC time>-<first 8 hex of SHA-256>.jpg and a JSON
sidecar with the header fields, so the file can be re-hashed against it later.
Only a camera image built with CONFIG_FIREX_CAM_BENCH_SHUTTER answers SNAP on its USB
console; the default image refuses it. See docs/media-policy.md.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxjpeg import FrameError, read_frame  # noqa: E402

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None

SIZES = ["qvga", "vga", "svga", "xga", "hd", "uxga"]


def ping(port) -> str | None:
    port.reset_input_buffer()
    port.write(b"PING\n")
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if line.startswith(b"FXPONG"):
            return line.decode("ascii", "replace")
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Pull one JPEG from the FireX ESP32-CAM bring-up image.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--size", choices=SIZES, default="vga")
    ap.add_argument("--quality", type=int, default=12, help="4 (best) to 63")
    ap.add_argument("--out", default="captures/camera")
    ap.add_argument("--retries", type=int, default=2)
    args = ap.parse_args()

    if serial is None:
        print("pyserial is required: pip install -r tools/capture/requirements.txt", file=sys.stderr)
        return 2

    with serial.Serial(args.port, args.baud, timeout=2) as port:
        time.sleep(0.3)  # opening the port can reset the board through DTR/RTS
        pong = ping(port) or ping(port)
        if not pong:
            print("no FXPONG: is the camera image flashed, and is IO0 released from GND?", file=sys.stderr)
            return 1
        print(pong)

        for attempt in range(1, args.retries + 2):
            port.reset_input_buffer()
            port.write(f"SNAP {args.size} {args.quality}\n".encode())
            try:
                jpeg, meta = read_frame(port)
            except FrameError as exc:
                print(f"attempt {attempt}: {exc}", file=sys.stderr)
                continue

            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            out = Path(args.out)
            out.mkdir(parents=True, exist_ok=True)
            path = out / f"snap-{stamp}-{meta['sha256'][:8]}.jpg"
            path.write_bytes(jpeg)
            meta.update({"captured_utc": stamp, "port": args.port, "camera": pong,
                         "size": args.size, "quality": args.quality})
            path.with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
            print(f"{path}  {meta['width']}x{meta['height']}  {meta['length']} bytes  sha256 {meta['sha256']}")
            return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())
