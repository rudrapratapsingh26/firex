"""
Parser for the node's bring-up serial protocol (firmware/main/app_main.c).

One record per line:

    H,t_ms,<column>,...        header, printed when a stream starts
    S,<t_ms>,<value>,...       one sample row matching the last header
    E,<t_ms>,{json}            an event: boot, self_test, calibration,
                               config_change, fan_state, mark

A console prompt may be glued to the front of a record ("firex> S,..."), and
ESP-IDF logs and command output share the port; anything that is not a record
is ignored. Standard library only, so it runs anywhere the tests run.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

RECORD = re.compile(r"^(?:\s*firex>\s*)*([HSE]),(.*)$")


@dataclass
class Header:
    columns: list[str]


@dataclass
class Sample:
    t_ms: int
    values: dict[str, str]


@dataclass
class Event:
    t_ms: int
    data: dict


@dataclass
class StreamParser:
    """Stateful: a sample is only meaningful against the header before it."""

    columns: list[str] | None = None
    dropped: int = 0
    events: list[Event] = field(default_factory=list)

    def feed(self, line: str) -> Header | Sample | Event | None:
        match = RECORD.match(line.rstrip("\r\n"))
        if not match:
            return None
        kind, rest = match.group(1), match.group(2)

        if kind == "H":
            columns = rest.split(",")
            if not columns or columns[0] != "t_ms":
                self.dropped += 1
                return None
            self.columns = columns
            return Header(columns)

        if kind == "S":
            fields = rest.split(",")
            if self.columns is None or len(fields) != len(self.columns):
                self.dropped += 1
                return None
            try:
                t_ms = int(fields[0])
            except ValueError:
                self.dropped += 1
                return None
            return Sample(t_ms, dict(zip(self.columns[1:], fields[1:])))

        t_text, _, payload = rest.partition(",")
        try:
            event = Event(int(t_text), json.loads(payload))
        except (ValueError, json.JSONDecodeError):
            self.dropped += 1
            return None
        self.events.append(event)
        return event


def number(value: str | None) -> float | None:
    """A CSV cell as a float, or None for an empty or non-numeric cell."""
    if value is None or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None
