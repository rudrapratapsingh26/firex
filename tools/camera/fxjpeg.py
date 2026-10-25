"""
Reader for the ESP32-CAM bring-up frame protocol (firmware/cam/main/cam_main.c).

    FXJPEG 1 <length> <sha256 hex> <width> <height>\\n
    <length raw JPEG bytes>
    \\nFXEND\\n
  or
    FXERR <reason>\\n

Any line that does not start with FX is skipped: boot messages and warnings
share the port. A frame is accepted only if exactly <length> bytes arrive, the
SHA-256 matches the header, and the bytes are a JPEG (SOI ... EOI). Standard
library only.
"""
from __future__ import annotations

import hashlib
from typing import Protocol


class Stream(Protocol):
    def readline(self) -> bytes: ...
    def read(self, size: int) -> bytes: ...


class FrameError(Exception):
    """The camera answered, but not with a usable frame."""


def _read_exact(stream: Stream, size: int) -> bytes:
    data = bytearray()
    stalls = 0
    while len(data) < size:
        chunk = stream.read(size - len(data))
        if not chunk:
            stalls += 1
            if stalls >= 3:
                raise FrameError(f"short frame: {len(data)} of {size} bytes")
            continue
        stalls = 0
        data.extend(chunk)
    return bytes(data)


def read_frame(stream: Stream, max_skip_lines: int = 200) -> tuple[bytes, dict]:
    for _ in range(max_skip_lines):
        line = stream.readline()
        if not line:
            continue
        text = line.strip()
        if text.startswith(b"FXERR"):
            raise FrameError(text[5:].strip().decode("ascii", "replace") or "unspecified")
        if not text.startswith(b"FXJPEG "):
            continue

        parts = text.decode("ascii", "replace").split()
        if len(parts) != 6 or parts[1] != "1":
            raise FrameError(f"unsupported header: {text!r}")
        try:
            length, width, height = int(parts[2]), int(parts[4]), int(parts[5])
        except ValueError as exc:
            raise FrameError(f"malformed header: {text!r}") from exc
        expected = parts[3].lower()
        if length <= 0 or len(expected) != 64:
            raise FrameError(f"malformed header: {text!r}")

        jpeg = _read_exact(stream, length)
        for _ in range(3):
            if stream.readline().strip() == b"FXEND":
                break
        else:
            raise FrameError("missing FXEND trailer")

        actual = hashlib.sha256(jpeg).hexdigest()
        if actual != expected:
            raise FrameError(f"sha256 mismatch: header {expected}, bytes {actual}")
        if not (jpeg.startswith(b"\xff\xd8") and jpeg.rstrip(b"\x00").endswith(b"\xff\xd9")):
            raise FrameError("payload is not a JPEG")
        return jpeg, {"version": 1, "length": length, "sha256": actual, "width": width, "height": height}

    raise FrameError("no frame header received")


def encode_frame(jpeg: bytes, width: int, height: int) -> bytes:
    """What the camera sends for one JPEG. Used by the tests."""
    digest = hashlib.sha256(jpeg).hexdigest()
    return f"FXJPEG 1 {len(jpeg)} {digest} {width} {height}\n".encode() + jpeg + b"\nFXEND\n"
