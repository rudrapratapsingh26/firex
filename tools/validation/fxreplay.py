"""
The replay harness: recorded captures in, fusion transitions out, error rates
against the trials marked on the bench.

A capture is what tools/capture/capture.py (or the burn-in runner) writes:
<name>.csv with the node's per-period scores and <name>.events.jsonl with its
E lines. Every sample is fed, in order, to the real compiled engine
(tools/simulator/fxengine.py) under a chosen configuration, and the replayed
states are scored against the trials:

    trial <class> <name>     a mark typed when an exposure starts (class hazard or nuisance)
    trial end                a mark typed when it stops

The category is the name before its first '-': `trial nuisance aerosol-deodorant`
is an aerosol trial. A labels CSV (capture,class,name,start_s,end_s) can supply
trials for a capture whose marks were missed; for that capture it replaces them.

What the numbers mean, all against the REPLAYED state, never the node's:

    window        [start, end + grace]
    hazard        detected if CRITICAL is reached in the window; tta_s = first CRITICAL - start
    nuisance      false_alarm if CRITICAL is reached in the window, else elevated if
                  ELEVATED or higher, else quiet
    background    time outside every window; each transition into CRITICAL there is a
                  background false alarm
    fidelity      the node's own state_change events against replayed transitions to
                  the same state within 2 x sample period + 1 s

The engine is fed the node's uptime clock (t_ms) and believes it booted at 0, so
the node's own warm-up applies after a reboot exactly as it did on the bench.
Replay works on the scores the node computed; settings that act inside a driver
(mq2_full, flame_quiet, heat_*) are not replayable and need a new capture.

Standard library only, apart from the engine binary itself.
"""
from __future__ import annotations

import csv
import hashlib
import itertools
import json
import statistics
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "simulator"))
import fxengine  # noqa: E402
import fxstats  # noqa: E402

SCORE_COLUMNS = [("gas", "gas_ok"), ("smoke", "smoke_ok"), ("flame", "flame_ok"), ("heat", "heat_ok"), ("rh", "rh_ok")]
STATE_RANK = {"WARMUP": 0, "NORMAL": 1, "ELEVATED": 2, "CRITICAL": 3}
CLASSES = ("hazard", "nuisance")
NUISANCE_CATEGORIES = ["cooking", "steam", "incense", "aerosol", "dust", "sunlight"]
HAZARD_CATEGORIES = ["gas", "smoke", "flame", "heat", "smoulder"]
GAP_FACTOR = 3            # a step longer than this many sample periods is a gap in the capture
BOOT_MATCH_S = 5.0        # an event whose boot offset is further than this from every segment is dropped


def category_of(cls: str, name: str) -> str:
    prefix = name.split("-", 1)[0].lower()
    known = HAZARD_CATEGORIES if cls == "hazard" else NUISANCE_CATEGORIES
    return prefix if prefix in known else "other"


# --- captures -----------------------------------------------------------------

@dataclass
class Sample:
    t_ms: int
    host_s: float | None
    scores: list[float]
    valid: list[bool]


@dataclass
class Segment:
    """Samples from one boot of the node: t_ms never runs backwards inside a segment."""
    index: int
    samples: list[Sample] = field(default_factory=list)
    events: list[dict] = field(default_factory=list)

    @property
    def start_ms(self) -> int:
        return self.samples[0].t_ms

    @property
    def end_ms(self) -> int:
        return self.samples[-1].t_ms

    def boot_offset_s(self) -> float | None:
        offsets = [s.host_s - s.t_ms / 1000.0 for s in self.samples if s.host_s is not None]
        return min(offsets) if offsets else None


@dataclass
class Trial:
    capture: str
    segment: int
    cls: str
    name: str
    start_ms: int
    end_ms: int
    closed: bool = True

    @property
    def category(self) -> str:
        return category_of(self.cls, self.name)


@dataclass
class Capture:
    path: Path
    sha256: str
    segments: list[Segment]
    trials: list[Trial]
    synthetic: bool
    period_ms: float | None
    gaps: int
    warnings: list[str] = field(default_factory=list)

    @property
    def name(self) -> str:
        return self.path.name

    @property
    def samples(self) -> int:
        return sum(len(s.samples) for s in self.segments)

    @property
    def duration_s(self) -> float:
        return sum((s.end_ms - s.start_ms) / 1000.0 for s in self.segments if s.samples)


def events_path_for(csv_path: Path) -> Path:
    name = csv_path.name
    stem = name[:-4] if name.lower().endswith(".csv") else name
    return csv_path.with_name(stem + ".events.jsonl")


def _float(text: str | None) -> float | None:
    if text is None or text == "":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def load_capture(csv_path: str | Path, events_path: str | Path | None = None,
                 labels: list[dict] | None = None) -> Capture:
    csv_path = Path(csv_path)
    raw = csv_path.read_bytes()
    warnings: list[str] = []

    segments: list[Segment] = []
    with open(csv_path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        columns = reader.fieldnames or []
        missing = [c for c, _ in SCORE_COLUMNS if c not in columns] + ([] if "t_ms" in columns else ["t_ms"])
        if missing:
            raise ValueError(f"{csv_path.name}: not a FireX capture, missing columns {', '.join(missing)}")
        previous_t = None
        for row in reader:
            t = _float(row.get("t_ms"))
            if t is None:
                continue
            scores, valid = [], []
            for score_col, ok_col in SCORE_COLUMNS:
                score = _float(row.get(score_col))
                if ok_col in columns:
                    is_valid = score is not None and str(row.get(ok_col) or "").strip() == "1"
                else:
                    is_valid = score is not None  # an older capture with no validity column
                scores.append(score if score is not None else 0.0)
                valid.append(is_valid)
            sample = Sample(int(t), _float(row.get("host_s")), scores, valid)
            if previous_t is None or sample.t_ms < previous_t:
                segments.append(Segment(len(segments)))
            segments[-1].samples.append(sample)
            previous_t = sample.t_ms
    if not segments:
        raise ValueError(f"{csv_path.name}: no samples")
    if len(segments) > 1:
        warnings.append(f"{csv_path.name}: the node's clock restarted {len(segments) - 1} time(s); replayed per boot")

    steps = [b.t_ms - a.t_ms for seg in segments for a, b in zip(seg.samples, seg.samples[1:]) if b.t_ms > a.t_ms]
    period = statistics.median(steps) if steps else None
    gaps = sum(1 for d in steps if period and d > GAP_FACTOR * period)
    if gaps:
        warnings.append(f"{csv_path.name}: {gaps} gap(s) longer than {GAP_FACTOR} sample periods")

    ev_path = Path(events_path) if events_path else events_path_for(csv_path)
    events = load_events(ev_path, warnings) if ev_path.exists() else []
    if not ev_path.exists():
        warnings.append(f"{csv_path.name}: no events file {ev_path.name}; no marks, no fidelity check")
    assign_events(segments, events, warnings, csv_path.name)
    synthetic = any(e.get("type") == "mark" and str(e.get("text", "")).lower().startswith("synthetic") for e in events)

    trials = trials_from_labels(csv_path.name, segments, labels, warnings) if labels \
        else trials_from_marks(csv_path.name, segments, warnings)
    return Capture(csv_path, hashlib.sha256(raw).hexdigest(), segments, trials, synthetic, period, gaps, warnings)


def load_events(path: Path, warnings: list[str]) -> list[dict]:
    events = []
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                warnings.append(f"{path.name}:{number}: not JSON, skipped")
                continue
            if isinstance(event, dict) and isinstance(event.get("t_ms"), (int, float)):
                events.append(event)
    return events


def assign_events(segments: list[Segment], events: list[dict], warnings: list[str], name: str) -> None:
    """Put each event in the boot it came from. With host times, a boot is identified by
    host_s - t_ms/1000, which stays constant for one boot; without them, by counting
    t_ms restarts in the event stream."""
    offsets = [seg.boot_offset_s() for seg in segments]
    dropped = 0
    if all(o is not None for o in offsets) and all(isinstance(e.get("host_s"), (int, float)) for e in events):
        for event in events:
            offset = event["host_s"] - event["t_ms"] / 1000.0
            best = min(range(len(segments)), key=lambda i: abs(offset - offsets[i]))
            if abs(offset - offsets[best]) > BOOT_MATCH_S:
                dropped += 1
                continue
            segments[best].events.append(event)
    else:
        index, previous = 0, None
        for event in events:
            if previous is not None and event["t_ms"] < previous:
                index += 1
            previous = event["t_ms"]
            if index < len(segments):
                segments[index].events.append(event)
            else:
                dropped += 1
    for seg in segments:
        seg.events.sort(key=lambda e: e["t_ms"])
    if dropped:
        warnings.append(f"{name}: {dropped} event(s) from a boot with no samples were ignored")


def trials_from_marks(capture: str, segments: list[Segment], warnings: list[str]) -> list[Trial]:
    trials: list[Trial] = []
    for seg in segments:
        open_trial: Trial | None = None
        for event in seg.events:
            if event.get("type") != "mark":
                continue
            words = str(event.get("text", "")).strip().split()
            if not words or words[0].lower() != "trial":
                continue
            t = int(event["t_ms"])
            if len(words) >= 2 and words[1].lower() == "end":
                if open_trial is None:
                    warnings.append(f"{capture}: 'trial end' at {t / 1000:.0f} s with no trial open")
                else:
                    open_trial.end_ms = t
                    trials.append(open_trial)
                    open_trial = None
                continue
            if len(words) >= 3 and words[1].lower() in CLASSES:
                if open_trial is not None:
                    warnings.append(f"{capture}: trial {open_trial.name} was not ended; closed when {words[2]} started")
                    open_trial.end_ms, open_trial.closed = t, False
                    trials.append(open_trial)
                open_trial = Trial(capture, seg.index, words[1].lower(), words[2], t, t)
                continue
            warnings.append(f"{capture}: mark '{event.get('text')}' not understood (trial hazard|nuisance <name>, or trial end)")
        if open_trial is not None:
            warnings.append(f"{capture}: trial {open_trial.name} was not ended; closed at the end of the capture")
            open_trial.end_ms, open_trial.closed = seg.end_ms, False
            trials.append(open_trial)
    return trials


def load_labels(path: str | Path) -> dict[str, list[dict]]:
    """capture,class,name,start_s,end_s -> {capture key: [rows]}. The key is matched
    against a capture's file name, its name without .csv, or its path."""
    labels: dict[str, list[dict]] = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for number, row in enumerate(csv.DictReader(handle), 2):
            cls = str(row.get("class", "")).strip().lower()
            if cls not in CLASSES:
                raise ValueError(f"{path}:{number}: class must be hazard or nuisance")
            start, end = _float(row.get("start_s")), _float(row.get("end_s"))
            if start is None or end is None or end < start:
                raise ValueError(f"{path}:{number}: start_s and end_s must be numbers with end_s >= start_s")
            key = str(row.get("capture", "")).strip().replace("\\", "/")
            labels.setdefault(key, []).append({"class": cls, "name": str(row.get("name", "")).strip() or "unnamed",
                                               "start_s": start, "end_s": end})
    return labels


def labels_for(labels: dict[str, list[dict]] | None, csv_path: Path) -> list[dict] | None:
    if not labels:
        return None
    name = csv_path.name
    keys = {name, name[:-4] if name.lower().endswith(".csv") else name, str(csv_path).replace("\\", "/")}
    for key, rows in labels.items():
        if key in keys or str(csv_path).replace("\\", "/").endswith("/" + key):
            return rows
    return None


def trials_from_labels(capture: str, segments: list[Segment], rows: list[dict], warnings: list[str]) -> list[Trial]:
    trials = []
    for row in rows:
        start, end = int(row["start_s"] * 1000), int(row["end_s"] * 1000)
        seg = next((s for s in segments if s.start_ms <= start <= s.end_ms), None)
        if seg is None:
            warnings.append(f"{capture}: labelled trial {row['name']} at {row['start_s']} s is outside every boot; "
                            f"placed in the first")
            seg = segments[0]
        trials.append(Trial(capture, seg.index, row["class"], row["name"], start, end))
    return trials


# --- replay ---------------------------------------------------------------------

Timeline = list[tuple[int, str]]
Transition = tuple[int, str, str, list[str]]


def replay_segment(settings: dict, segment: Segment) -> tuple[Timeline, list[Transition]]:
    with fxengine.Engine(settings) as engine:
        results = engine.steps([(s.t_ms, s.scores, s.valid) for s in segment.samples])
    timeline: Timeline = []
    transitions: list[Transition] = []
    previous = "WARMUP"
    for sample, result in zip(segment.samples, results):
        state = fxengine.STATES[result["state"]]
        if state != previous:
            transitions.append((sample.t_ms, previous, state, fxengine.reason_names(result)))
            previous = state
        timeline.append((sample.t_ms, state))
    return timeline, transitions


def transitions_of(timeline: Timeline, initial: str = "WARMUP") -> list[Transition]:
    out: list[Transition] = []
    previous = initial
    for t, state in timeline:
        if state != previous:
            out.append((t, previous, state, []))
            previous = state
    return out


# --- scoring (pure: no engine) ---------------------------------------------------

def evaluate_trial(trial: Trial, timeline: Timeline, grace_ms: int) -> dict:
    window_end = trial.end_ms + grace_ms
    inside = [(t, s) for t, s in timeline if trial.start_ms <= t <= window_end]
    critical_at = next((t for t, s in inside if s == "CRITICAL"), None)
    elevated_at = next((t for t, s in inside if STATE_RANK[s] >= STATE_RANK["ELEVATED"]), None)
    max_state = max((s for _, s in inside), key=lambda s: STATE_RANK[s]) if inside else None
    if trial.cls == "hazard":
        outcome = "detected" if critical_at is not None else "missed"
    else:
        outcome = "false_alarm" if critical_at is not None else "elevated" if elevated_at is not None else "quiet"
    return {
        "capture": trial.capture,
        "segment": trial.segment,
        "class": trial.cls,
        "category": trial.category,
        "name": trial.name,
        "start_s": trial.start_ms / 1000.0,
        "end_s": trial.end_ms / 1000.0,
        "closed": trial.closed,
        "samples": len(inside),
        "max_state": max_state,
        "critical_at_s": None if critical_at is None else critical_at / 1000.0,
        "elevated_at_s": None if elevated_at is None else elevated_at / 1000.0,
        "tta_s": None if critical_at is None or trial.cls != "hazard" else (critical_at - trial.start_ms) / 1000.0,
        "outcome": outcome,
    }


def background(timeline: Timeline, windows: list[tuple[int, int]], period_ms: float | None) -> tuple[float, int]:
    """(milliseconds outside every window, transitions into CRITICAL outside every window)."""
    def outside(t: int) -> bool:
        return not any(a <= t <= b for a, b in windows)

    cap = GAP_FACTOR * period_ms if period_ms else None
    ms, false_alarms = 0.0, 0
    for i, (t, state) in enumerate(timeline):
        if i + 1 < len(timeline):
            dt = timeline[i + 1][0] - t
            if cap is not None:
                dt = min(dt, cap)
            if outside(t):
                ms += max(0, dt)
        if i > 0 and state == "CRITICAL" and timeline[i - 1][1] != "CRITICAL" and outside(t):
            false_alarms += 1
    return ms, false_alarms


def fidelity(events: list[dict], transitions: list[Transition], period_ms: float | None,
             span: tuple[int, int]) -> dict:
    tolerance = 2 * (period_ms or 0) + 1000
    node = [e for e in events if e.get("type") == "state_change" and isinstance(e.get("to"), str)
            and span[0] - tolerance <= e["t_ms"] <= span[1] + tolerance]
    unmatched = []
    matched = 0
    for event in node:
        if any(to == event["to"] and abs(t - event["t_ms"]) <= tolerance for t, _, to, _ in transitions):
            matched += 1
        elif len(unmatched) < 20:
            unmatched.append({"t_s": event["t_ms"] / 1000.0, "to": event["to"]})
    return {
        "node_transitions": len(node),
        "matched": matched,
        "replay_transitions": len(transitions),
        "config_changes_in_capture": sum(1 for e in events if e.get("type") == "config_change"),
        "tolerance_ms": tolerance,
        "unmatched": unmatched,
    }


def summarize_trials(trials: list[dict]) -> tuple[dict, dict]:
    hazards = [t for t in trials if t["class"] == "hazard"]
    nuisances = [t for t in trials if t["class"] == "nuisance"]

    def hazard_block(group: list[dict]) -> dict:
        detected = sum(1 for t in group if t["outcome"] == "detected")
        return {"detection": fxstats.rate(detected, len(group)),
                "miss": fxstats.rate(len(group) - detected, len(group)),
                "tta_s": fxstats.summarize([t["tta_s"] for t in group if t["tta_s"] is not None])}

    def nuisance_block(group: list[dict]) -> dict:
        return {"false_alarm": fxstats.rate(sum(1 for t in group if t["outcome"] == "false_alarm"), len(group)),
                "elevated": fxstats.rate(sum(1 for t in group if t["outcome"] == "elevated"), len(group))}

    hazard = hazard_block(hazards)
    categories = HAZARD_CATEGORIES + sorted({t["category"] for t in hazards} - set(HAZARD_CATEGORIES))
    hazard["by_category"] = {}
    for cat in categories:
        block = hazard_block([t for t in hazards if t["category"] == cat])
        hazard["by_category"][cat] = {"detection": block["detection"], "tta_s": block["tta_s"]}

    nuisance = nuisance_block(nuisances)
    categories = NUISANCE_CATEGORIES + sorted({t["category"] for t in nuisances} - set(NUISANCE_CATEGORIES))
    nuisance["by_category"] = {cat: nuisance_block([t for t in nuisances if t["category"] == cat]) for cat in categories}
    return hazard, nuisance


# --- configurations --------------------------------------------------------------

def parse_assignment(text: str) -> tuple[str, int]:
    key, sep, value = text.partition("=")
    key = key.strip()
    if not sep or key not in fxengine.DEFAULT_SETTINGS:
        raise ValueError(f"'{text}': expected fx_key=value with a key from {', '.join(fxengine.DEFAULT_SETTINGS)}")
    try:
        return key, int(value)
    except ValueError:
        raise ValueError(f"'{text}': values are integers in the node's units (hundredths, ms, s)") from None


def parse_sweep(text: str) -> tuple[str, list[int]]:
    key, _, values = text.partition("=")
    parsed = [parse_assignment(f"{key}={v}")[1] for v in values.split(",") if v.strip()]
    if not parsed:
        raise ValueError(f"'{text}': expected fx_key=v1,v2")
    return key.strip(), parsed


def config_name(settings: dict) -> str:
    diffs = [f"{k}={v}" for k, v in settings.items() if fxengine.DEFAULT_SETTINGS.get(k) != v]
    return ", ".join(diffs) if diffs else "node defaults"


def build_configs(base: dict, sweeps: list[tuple[str, list[int]]]) -> list[tuple[str, dict]]:
    """The base settings, or every combination of the swept values over it."""
    base = fxengine.merged(base)
    if not sweeps:
        return [(config_name(base), base)]
    configs = []
    for combo in itertools.product(*[values for _, values in sweeps]):
        settings = dict(base)
        settings.update({key: value for (key, _), value in zip(sweeps, combo)})
        configs.append((config_name(settings), settings))
    return configs


# --- the whole run ---------------------------------------------------------------

def replay(captures: list[Capture], configs: list[tuple[str, dict]], grace_s: float = 60.0,
           progress=None) -> dict:
    grace_ms = int(grace_s * 1000)
    warnings = [w for c in captures for w in c.warnings]
    doc_configs, skipped = [], []
    for name, settings in configs:
        why = fxengine.check_settings(settings)
        if why:
            skipped.append({"name": name, "settings": settings, "reason": why})
            continue
        if progress:
            progress(f"replaying {len(captures)} capture(s) with {name}")
        trials: list[dict] = []
        bg_ms, bg_false = 0.0, 0
        fid = {"node_transitions": 0, "matched": 0, "replay_transitions": 0, "config_changes_in_capture": 0,
               "unmatched": []}
        for capture in captures:
            for seg in capture.segments:
                timeline, transitions = replay_segment(settings, seg)
                seg_trials = [t for t in capture.trials if t.segment == seg.index]
                trials += [evaluate_trial(t, timeline, grace_ms) for t in seg_trials]
                ms, fa = background(timeline, [(t.start_ms, t.end_ms + grace_ms) for t in seg_trials],
                                    capture.period_ms)
                bg_ms, bg_false = bg_ms + ms, bg_false + fa
                f = fidelity(seg.events, transitions, capture.period_ms, (seg.start_ms, seg.end_ms))
                for key in ("node_transitions", "matched", "replay_transitions", "config_changes_in_capture"):
                    fid[key] += f[key]
                fid["unmatched"] += [{"capture": capture.name, **u} for u in f["unmatched"]]
        fid["unmatched"] = fid["unmatched"][:20]
        hours = bg_ms / 3_600_000.0
        hazard, nuisance = summarize_trials(trials)
        doc_configs.append({
            "name": name,
            "settings": settings,
            "hazard": hazard,
            "nuisance": nuisance,
            "background": {"hours": hours, "false_alarms": bg_false,
                           "per_24h": (bg_false / hours * 24.0) if hours > 0 else None},
            "fidelity": fid,
            "trials": trials,
        })
    if any(c.segments and any(e.get("type") == "config_change" for s in c.segments for e in s.events) for c in captures):
        warnings.append("settings changed during at least one capture: replay runs one configuration throughout, "
                        "so fidelity for that capture is not expected to be exact")
    return {
        "kind": "replay",
        "synthetic": any(c.synthetic for c in captures),
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "engine": {"binary": str(fxengine.binary_path()), "sha256": fxengine.binary_sha256()},
        "grace_s": grace_s,
        "captures": [{"path": str(c.path), "sha256": c.sha256, "samples": c.samples, "duration_s": c.duration_s,
                      "period_ms": c.period_ms, "gaps": c.gaps, "boots": len(c.segments), "trials": len(c.trials),
                      "synthetic": c.synthetic} for c in captures],
        "configs": doc_configs,
        "skipped_configs": skipped,
        "warnings": warnings,
    }
