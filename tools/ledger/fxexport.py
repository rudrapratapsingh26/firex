"""
Parser for the node's ledger and media export lines (firmware/main/ledger_task.c
and camera_task.c), and the log bundle the reference verifier reads.

    X,begin,<device_id>
    X,key,<PEM with newlines written as \\n>
    X,entry,<EntryJson>
    X,end,<count>

    M,file,<name>,<size>,<sha256>
    M,begin,<name>,<size>
    M,data,<hex>
    M,end,<sha256>
    M,error,<message>

A console prompt may be glued to the front of a line. Standard library only.
"""
from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field

PROMPT = re.compile(r"^(?:\s*firex>\s*)*")


class ExportError(Exception):
    pass


@dataclass
class MediaFile:
    name: str
    size: int
    sha256: str
    data: bytes = b""


@dataclass
class ExportParser:
    device_id: str | None = None
    public_key_pem: str | None = None
    entries: list[dict] = field(default_factory=list)
    log_done: bool = False
    listed: dict[str, MediaFile] = field(default_factory=dict)
    files: dict[str, MediaFile] = field(default_factory=dict)
    _current: MediaFile | None = None
    _chunks: list[bytes] = field(default_factory=list)

    def feed(self, raw: str) -> str | None:
        """Consume one line. Returns the record kind handled, or None for other output."""
        line = PROMPT.sub("", raw.rstrip("\r\n"))
        if line.startswith("X,"):
            return self._log_line(line)
        if line.startswith("M,"):
            return self._media_line(line)
        return None

    def _log_line(self, line: str) -> str:
        kind, _, rest = line[2:].partition(",")
        if kind == "begin":
            self.device_id, self.entries, self.log_done = rest, [], False
        elif kind == "key":
            self.public_key_pem = rest.replace("\\n", "\n")
        elif kind == "entry":
            try:
                self.entries.append(json.loads(rest))
            except json.JSONDecodeError as exc:
                raise ExportError(f"entry {len(self.entries)} is not valid JSON: {exc}") from exc
        elif kind == "end":
            if int(rest) != len(self.entries):
                raise ExportError(f"node reported {rest} entries, {len(self.entries)} arrived")
            self.log_done = True
        return f"X,{kind}"

    def _media_line(self, line: str) -> str:
        kind, _, rest = line[2:].partition(",")
        if kind == "file":
            name, size, sha = rest.rsplit(",", 2)
            self.listed[name] = MediaFile(name, int(size), sha)
        elif kind == "begin":
            name, size = rest.rsplit(",", 1)
            self._current, self._chunks = MediaFile(name, int(size), ""), []
        elif kind == "data":
            if self._current is None:
                raise ExportError("media data before M,begin")
            self._chunks.append(bytes.fromhex(rest))
        elif kind == "end":
            if self._current is None:
                raise ExportError("M,end before M,begin")
            data = b"".join(self._chunks)
            actual = hashlib.sha256(data).hexdigest()
            if actual != rest or len(data) != self._current.size:
                raise ExportError(f"{self._current.name}: transfer damaged (sha256 {actual}, node said {rest})")
            self._current.sha256, self._current.data = actual, data
            self.files[self._current.name] = self._current
            self._current = None
        elif kind == "error":
            raise ExportError(f"node: {rest}")
        return f"M,{kind}"

    def bundle(self) -> dict:
        """The verifier's log bundle. Media hashes come from files actually received."""
        if not self.log_done:
            raise ExportError("the log export did not finish")
        if not self.public_key_pem:
            raise ExportError("the export carried no device key")
        return {
            "device_id": self.device_id,
            "public_key_pem": self.public_key_pem,
            "media": sorted(f.sha256 for f in self.files.values()),
            "log": self.entries,
        }


def export_lines(vector: dict) -> list[str]:
    """The lines a node would print for a log bundle. Used by the tests."""
    lines = [f"X,begin,{vector['log'][0]['device_id'] if vector['log'] else ''}",
             "X,key," + vector["public_key_pem"].replace("\n", "\\n")]
    lines += ["X,entry," + json.dumps(e, separators=(",", ":")) for e in vector["log"]]
    lines.append(f"X,end,{len(vector['log'])}")
    return lines
