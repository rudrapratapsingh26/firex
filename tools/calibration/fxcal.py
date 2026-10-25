"""
Characterisation helpers for M1 bench captures.

Input is what tools/capture/capture.py writes: <name>.csv with the node's own
column header, and <name>.events.jsonl with marks, fan_state and calibration
events. The notebooks in this directory call these functions; the functions are
plain Python with no third-party dependency, so tools/calibration/test_fxcal.py
checks the arithmetic in CI.

The demo_* functions generate SYNTHETIC captures so every notebook runs before
the dev kit exists. Their numbers are made up. Nothing derived from them is a
measurement.
"""
from __future__ import annotations

import csv
import json
import math
import random
import statistics
from dataclasses import dataclass
from pathlib import Path

Capture = dict[str, list]


# --- loading --------------------------------------------------------------

def _cell(text: str | None):
    if text is None or text == "":
        return None
    try:
        return float(text)
    except ValueError:
        return text


def load_capture(path: str | Path) -> Capture:
    """Columns by name. Numbers become floats, blanks None, status words stay text.
    A t_s column (device seconds since boot) is added from t_ms."""
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        names = list(reader.fieldnames or [])
        columns: Capture = {name: [] for name in names}
        for row in reader:
            for name in names:
                columns[name].append(_cell(row.get(name)))
    if "t_ms" in columns:
        columns["t_s"] = [None if v is None else v / 1000.0 for v in columns["t_ms"]]
    return columns


def load_events(path: str | Path) -> list[dict]:
    events = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                event = json.loads(line)
                event["t_s"] = event.get("t_ms", 0) / 1000.0
                events.append(event)
    return events


def series(cap: Capture, column: str, valid: str | None = None,
           t_from: float | None = None, t_to: float | None = None) -> tuple[list[float], list[float]]:
    """(times, values) for one numeric column, skipping blanks, rows whose
    validity column is not 1, and rows outside [t_from, t_to]."""
    ts, vs = [], []
    times = cap.get("t_s", [])
    values = cap.get(column, [])
    flags = cap.get(valid, []) if valid else None
    for i, (t, v) in enumerate(zip(times, values)):
        if t is None or not isinstance(v, float):
            continue
        if flags is not None and flags[i] != 1.0:
            continue
        if (t_from is not None and t < t_from) or (t_to is not None and t > t_to):
            continue
        ts.append(t)
        vs.append(v)
    return ts, vs


# --- warm-up and baselines ------------------------------------------------

def warmup_time(ts: list[float], vs: list[float], tolerance: float = 0.02, hold_s: float = 60.0) -> float | None:
    """First time after which the signal stays within `tolerance` (relative
    spread, max - min over the median) for a full `hold_s` window."""
    j = 0
    for i in range(len(ts)):
        j = max(j, i)
        while j + 1 < len(ts) and ts[j + 1] <= ts[i] + hold_s:
            j += 1
        if ts[j] - ts[i] < hold_s * 0.95:
            return None  # the capture ends before a full window
        window = vs[i:j + 1]
        middle = statistics.median(window)
        if middle and (max(window) - min(window)) / abs(middle) <= tolerance:
            return ts[i]
    return None


def baseline(values: list[float]) -> tuple[float, float]:
    """(median, spread) with spread = (p90 - p10) / median: the same rule the
    firmware applies in fx_mq_baseline_add, so notebook and device agree."""
    if not values:
        raise ValueError("no values")
    ordered = sorted(values)
    n = len(ordered)
    middle = statistics.median(ordered)
    tail = n // 10
    spread = (ordered[n - 1 - tail] - ordered[tail]) / middle if middle else math.inf
    return middle, spread


# --- step responses -------------------------------------------------------

@dataclass
class Response:
    t_start: float
    baseline: float
    extreme: float
    direction: str  # "rise" or "fall"
    t10: float | None
    t50: float | None
    t90: float | None


def response_times(ts: list[float], vs: list[float], t_start: float, t_end: float | None = None,
                   pre_s: float = 30.0) -> Response:
    """Rise (or fall) times after an exposure starts at t_start: seconds until
    the signal covers 10, 50 and 90 % of the way from its pre-exposure median
    to its extreme before t_end. Crossings are interpolated between samples."""
    pre = [v for t, v in zip(ts, vs) if t_start - pre_s <= t < t_start]
    during = [(t, v) for t, v in zip(ts, vs) if t >= t_start and (t_end is None or t <= t_end)]
    if not pre or not during:
        raise ValueError("need samples both before and after t_start")

    base = statistics.median(pre)
    high = max(v for _, v in during)
    low = min(v for _, v in during)
    rising = (high - base) >= (base - low)
    extreme = high if rising else low
    amplitude = extreme - base
    result = Response(t_start, base, extreme, "rise" if rising else "fall", None, None, None)
    if abs(amplitude) <= 1e-12 or abs(amplitude) < 1e-3 * max(abs(base), 1e-12):
        return result

    def crossing(fraction: float) -> float | None:
        threshold = base + fraction * amplitude
        prev_t, prev_v = t_start, base
        for t, v in during:
            if (v - threshold) * (1 if rising else -1) >= 0:
                if v == prev_v:
                    return t - t_start
                return prev_t + (threshold - prev_v) / (v - prev_v) * (t - prev_t) - t_start
            prev_t, prev_v = t, v
        return None

    result.t10, result.t50, result.t90 = crossing(0.1), crossing(0.5), crossing(0.9)
    return result


def mark_pairs(events: list[dict], start: str = "gas in", end: str = "gas out") -> list[tuple[float, float | None]]:
    """Exposure windows from mark events, in device seconds."""
    pairs: list[tuple[float, float | None]] = []
    open_start = None
    for event in events:
        if event.get("type") != "mark":
            continue
        text = str(event.get("text", "")).strip().lower()
        if text == start:
            if open_start is not None:
                pairs.append((open_start, None))
            open_start = event["t_s"]
        elif text == end and open_start is not None:
            pairs.append((open_start, event["t_s"]))
            open_start = None
    if open_start is not None:
        pairs.append((open_start, None))
    return pairs


def fan_state_at(events: list[dict], t_s: float) -> str | None:
    """The fan state in force at t_s, from fan_state events."""
    state = None
    for event in events:
        if event.get("type") == "fan_state" and event["t_s"] <= t_s:
            state = event.get("state")
    return state


def compare_fan(on: list[Response], off: list[Response], key: str = "t90") -> dict:
    """Median response time with the fan on and off, and how they differ."""
    on_times = [getattr(r, key) for r in on if getattr(r, key) is not None]
    off_times = [getattr(r, key) for r in off if getattr(r, key) is not None]
    summary = {"key": key, "n_on": len(on_times), "n_off": len(off_times),
               "on_median_s": None, "off_median_s": None, "difference_s": None, "ratio": None}
    if on_times:
        summary["on_median_s"] = statistics.median(on_times)
    if off_times:
        summary["off_median_s"] = statistics.median(off_times)
    if on_times and off_times:
        summary["difference_s"] = summary["off_median_s"] - summary["on_median_s"]
        if summary["on_median_s"] > 0:
            summary["ratio"] = summary["off_median_s"] / summary["on_median_s"]
    return summary


def linear_fit(xs: list[float], ys: list[float]) -> tuple[float, float, float]:
    """Least squares (slope, intercept, r squared)."""
    n = len(xs)
    if n < 2:
        raise ValueError("need at least two points")
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0:
        raise ValueError("x has no spread")
    slope = sxy / sxx
    r2 = (sxy * sxy) / (sxx * syy) if syy else 1.0
    return slope, my - slope * mx, r2


# --- synthetic demo data (NOT measurements) --------------------------------

def synthetic_step(t0: float, tau: float, base: float, extreme: float, duration: float,
                   dt: float = 1.0, start: float = 0.0, noise: float = 0.0, seed: int = 0):
    """A first-order step from base to extreme at t0, sampled every dt."""
    rng = random.Random(seed)
    ts, vs = [], []
    t = start
    while t <= duration:
        v = base if t < t0 else extreme + (base - extreme) * math.exp(-(t - t0) / tau)
        ts.append(round(t, 6))
        vs.append(v + (rng.uniform(-noise, noise) if noise else 0.0))
        t += dt
    return ts, vs


def demo_warmup() -> Capture:
    """SYNTHETIC cold start: Rs falls toward its clean-air value as the heater warms."""
    rng = random.Random(1)
    ts = [float(t) for t in range(0, 1201, 2)]
    return {
        "t_s": ts,
        "mq2_rs": [9000.0 * (1 + 2.5 * math.exp(-t / 110)) * (1 + rng.uniform(-0.003, 0.003)) for t in ts],
        "mq135_rs": [42000.0 * (1 + 1.2 * math.exp(-t / 180)) * (1 + rng.uniform(-0.003, 0.003)) for t in ts],
    }


def demo_fan_runs() -> tuple[Capture, list[dict]]:
    """SYNTHETIC: four butane puffs, two with the fan on (tau 8 s) and two off (tau 26 s)."""
    runs = [("on", 8.0), ("off", 26.0), ("on", 8.5), ("off", 24.0)]
    ts: list[float] = []
    ratio: list[float] = []
    events: list[dict] = []
    t = 0.0
    rng = random.Random(3)
    for index, (state, tau) in enumerate(runs):
        events.append({"type": "fan_state", "state": state, "duty": 100 if state == "on" else 0, "t_s": t})
        t_in, t_out, t_end = t + 60.0, t + 180.0, t + 400.0
        events.append({"type": "mark", "text": "gas in", "t_s": t_in})
        events.append({"type": "mark", "text": "gas out", "t_s": t_out})
        while t < t_end:
            if t < t_in:
                v = 1.0
            elif t < t_out:
                v = 0.3 + 0.7 * math.exp(-(t - t_in) / tau)
            else:
                at_out = 0.3 + 0.7 * math.exp(-(t_out - t_in) / tau)
                v = 1.0 + (at_out - 1.0) * math.exp(-(t - t_out) / (tau * 3))
            ts.append(t)
            ratio.append(v * (1 + rng.uniform(-0.004, 0.004)))
            t += 1.0
        del index
    return {"t_s": ts, "mq2_ratio": ratio}, events


def demo_rh() -> Capture:
    """SYNTHETIC: clean air while relative humidity is swept from 40 to 90 %."""
    rng = random.Random(5)
    rh = [40.0 + 50.0 * i / 299 for i in range(300)]
    return {
        "t_s": [2.0 * i for i in range(300)],
        "rh_pct": rh,
        "mq2_ratio": [1.0 - 0.0045 * (h - 40.0) + rng.uniform(-0.01, 0.01) for h in rh],
        "mq135_ratio": [1.0 - 0.0060 * (h - 40.0) + rng.uniform(-0.01, 0.01) for h in rh],
    }
