"""
Alert-path latency on the bench: from the node's threshold crossing to an alert
arriving at a mock emergency-service endpoint, split into the steps that add up
to it.

    python tools/validation/alert_latency.py --capture captures/m4-alerts.events.jsonl \\
        --csv captures/m4-alerts.csv --endpoints endpoints.jsonl --out reports/alert-latency-bench

Each step is timed on the clocks that can time it:

    crossing -> CRITICAL        the node's t_ms at both ends
    CRITICAL -> gateway stored  node time mapped onto the capture host's wall clock, against the gateway
    gateway  -> delivered       the gateway's and the mock endpoints' clocks

The node's ledger timestamps are `uncertain` (there is no clock sync yet), so its
t_ms is mapped onto the host's wall clock from the capture itself: for each boot,
the offset is the smallest (host_unix_ms - t_ms) over every line recorded,
because serial delay only ever makes a line arrive later. The mapping is
comparable with the gateway's timestamps only when the capture, the gateway and
the mock endpoints run on one machine, as in docs/bringup/m4-bench.md; otherwise
give the measured difference (gateway clock minus capture clock) as
--clock-offset-ms.

Standard library only. demo_files() writes SYNTHETIC inputs; nothing derived from
them is a measurement.
"""
from __future__ import annotations

import csv
import hashlib
import json
import random
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxstats  # noqa: E402

REBOOT_DROP_MS = 30_000


# --- loading -------------------------------------------------------------------

def load_jsonl(path: str | Path) -> list[dict]:
    records = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    continue  # a line cut short when the host stopped
    return records


def csv_host_times(path: str | Path) -> list[tuple[int, int]]:
    """(host_unix_ms, t_ms) for every sample row of a capture. Reads only those two columns."""
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if not header:
            return []
        if "host_unix_ms" not in header:
            raise ValueError(f"{path} has no host_unix_ms column: record it with the M4 capture tools")
        i_host, i_t = header.index("host_unix_ms"), header.index("t_ms")
        rows = []
        for row in reader:
            try:
                rows.append((int(row[i_host]), int(row[i_t])))
            except (ValueError, IndexError):
                continue
        return rows


def sha256_file(path: str | Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_synthetic(events: list[dict]) -> bool:
    """Generated inputs carry a mark whose text starts with SYNTHETIC."""
    return any(e.get("type") == "mark" and str(e.get("text", "")).upper().startswith("SYNTHETIC") for e in events)


# --- the node's clock ------------------------------------------------------------

def boot_segments(points: list[tuple[int, int, bool]], drop_ms: int = REBOOT_DROP_MS) -> list[int]:
    """Boot segment of each (host_unix_ms, t_ms, is_boot_event) point, points in host order.

    A `boot` echo starts a new segment. So does the device clock falling more than
    drop_ms below the segment's highest t_ms, for a reboot whose boot echo was lost.
    Smaller steps backwards are normal: a ledger echo carries the time its event was
    queued, and can arrive after stream lines printed later."""
    out: list[int] = []
    segment, seen_any, highest = 0, False, None
    for _, t, is_boot in points:
        if seen_any and (is_boot or (highest is not None and t < highest - drop_ms)):
            segment += 1
            highest = None
        seen_any = True
        out.append(segment)
        highest = t if highest is None else max(highest, t)
    return out


def map_device_time(events: list[dict], samples: list[tuple[int, int]]) -> tuple[list[dict], dict[int, int]]:
    """Events in host order, each copied with `segment` and `device_host_ms` (its t_ms on
    the host's wall clock), and the offset used for each segment."""
    points = [(h, t, False, None) for h, t in samples]
    points += [(int(e["host_unix_ms"]), int(e["t_ms"]), e.get("type") == "boot", e)
               for e in events if isinstance(e.get("host_unix_ms"), (int, float)) and isinstance(e.get("t_ms"), (int, float))]
    points.sort(key=lambda p: p[0])
    segments = boot_segments([(h, t, b) for h, t, b, _ in points])
    offsets: dict[int, int] = {}
    for (h, t, _, _), seg in zip(points, segments):
        offsets[seg] = min(offsets.get(seg, h - t), h - t)
    mapped = []
    for (h, t, _, e), seg in zip(points, segments):
        if e is not None:
            mapped.append({**e, "segment": seg, "device_host_ms": t + offsets[seg]})
    return mapped, offsets


def incidents(mapped: list[dict]) -> list[dict]:
    """Each transition into CRITICAL, with the start of the agreement that caused it: for
    each reason modality the latest upward threshold_cross before the transition in the
    same boot, then the earliest of those."""
    crossings = [e for e in mapped if e.get("type") == "threshold_cross" and e.get("direction") == "up"]
    out = []
    for e in mapped:
        if e.get("type") != "state_change" or e.get("to") != "CRITICAL":
            continue
        reasons = [str(r) for r in (e.get("reason") or [])]
        latest: dict[str, dict] = {}
        for c in crossings:
            m = str(c.get("modality"))
            if c["segment"] == e["segment"] and m in reasons and c["t_ms"] <= e["t_ms"]:
                if m not in latest or c["t_ms"] >= latest[m]["t_ms"]:
                    latest[m] = c
        start = min(latest.values(), key=lambda c: c["t_ms"]) if latest else None
        out.append({
            "seq": e.get("seq"), "t_ms": e["t_ms"], "segment": e["segment"], "host_ms": e["device_host_ms"],
            "reason": reasons, "crossing_t_ms": start["t_ms"] if start else None,
            "crossing_host_ms": start["device_host_ms"] if start else None,
            "crossing_modality": start.get("modality") if start else None,
        })
    return out


# --- the join -------------------------------------------------------------------

def analyse(events_path: str | Path, csv_path: str | Path, endpoints_path: str | Path, device: str | None = None,
            ack_timeout_s: float = 60.0, clock_offset_ms: int = 0, services: list[str] | None = None,
            synthetic: bool = False) -> dict:
    events = load_jsonl(events_path)
    samples = csv_host_times(csv_path) if csv_path else []
    records = load_jsonl(endpoints_path)
    synthetic = synthetic or is_synthetic(events)
    mapped, offsets = map_device_time(events, samples)
    found = incidents(mapped)

    delivered = [r for r in records if r.get("event") == "delivered"
                 and (device is None or str(r.get("node_id", "")).lower() == device.lower())]
    by_receipt = {r["receipt_id"]: r for r in records if r.get("event") == "delivered" and "receipt_id" in r}
    acks = {r["receipt_id"]: r for r in records if r.get("event") == "acked" and "receipt_id" in r}
    expected = sorted(services) if services else sorted({str(r.get("service")) for r in delivered})

    groups: dict[tuple, list[dict]] = {}
    for r in delivered:
        groups.setdefault((r.get("incident_id"), r.get("node_id"), r.get("ledger_seq")), []).append(r)
    used: set[tuple] = set()

    samples_out, failures = [], []
    seg = {"crossing_to_critical_ms": [], "critical_to_gateway_ms": [], "gateway_to_delivered_ms": [],
           "crossing_to_delivered_ms": []}
    per_service: dict[str, dict] = {}
    realert_observed: list[float] = []

    for inc in found:
        if inc["crossing_t_ms"] is not None:
            seg["crossing_to_critical_ms"].append(inc["t_ms"] - inc["crossing_t_ms"])
        candidates = [k for k in groups if k not in used and k[2] == inc["seq"]]
        sample = {"seq": inc["seq"], "host_unix_ms": inc["host_ms"], "reason": inc["reason"],
                  "crossing_modality": inc["crossing_modality"],
                  "crossing_to_critical_ms": None if inc["crossing_t_ms"] is None else inc["t_ms"] - inc["crossing_t_ms"],
                  "incident_id": None, "critical_to_gateway_ms": None, "deliveries": {}}
        if not candidates:
            failures.append({"seq": inc["seq"], "host_unix_ms": inc["host_ms"], "reason": "no delivery recorded"})
            samples_out.append(sample)
            continue
        critical_gw = inc["host_ms"] + clock_offset_ms

        def distance(key: tuple) -> float:
            received = min(int(r.get("ledger_received_at_ms", 0)) for r in groups[key])
            return abs(received - critical_gw)

        key = min(candidates, key=distance)
        used.add(key)
        group = groups[key]
        originals = sorted((r for r in group if not r.get("retry_of")), key=lambda r: r["received_at_ms"])
        ledger_received = min(int(r["ledger_received_at_ms"]) for r in group)
        sample["incident_id"] = key[0]
        sample["critical_to_gateway_ms"] = ledger_received - critical_gw
        seg["critical_to_gateway_ms"].append(ledger_received - critical_gw)

        first_by_service: dict[str, dict] = {}
        for r in originals:
            first_by_service.setdefault(str(r.get("service")), r)
        for service, r in first_by_service.items():
            gw_to_delivered = int(r["received_at_ms"]) - int(r["ledger_received_at_ms"])
            crossing_to_delivered = (None if inc["crossing_host_ms"] is None
                                     else int(r["received_at_ms"]) - (inc["crossing_host_ms"] + clock_offset_ms))
            ack = acks.get(r.get("receipt_id"))
            ack_s = None if ack is None else (int(ack["at_ms"]) - int(r["received_at_ms"])) / 1000.0
            sample["deliveries"][service] = {"receipt_id": r.get("receipt_id"), "received_at_ms": r["received_at_ms"],
                                             "gateway_to_delivered_ms": gw_to_delivered,
                                             "crossing_to_delivered_ms": crossing_to_delivered, "ack_latency_s": ack_s}
            seg["gateway_to_delivered_ms"].append(gw_to_delivered)
            stats = per_service.setdefault(service, {"delivered": 0, "_crossing": [], "_ack": []})
            stats["delivered"] += 1
            if crossing_to_delivered is not None:
                seg["crossing_to_delivered_ms"].append(crossing_to_delivered)
                stats["_crossing"].append(crossing_to_delivered)
            if ack_s is not None:
                stats["_ack"].append(ack_s)
        for service in expected:
            if service not in first_by_service:
                failures.append({"seq": inc["seq"], "host_unix_ms": inc["host_ms"], "reason": f"no delivery to {service}"})

        for r in group:
            target = by_receipt.get(r.get("retry_of")) if r.get("retry_of") else None
            if target is not None:
                realert_observed.append((int(r["received_at_ms"]) - int(target["received_at_ms"])) / 1000.0)
        samples_out.append(sample)

    unmatched = sum(1 for k, g in groups.items() if k not in used for r in g if not r.get("retry_of"))
    inputs = [{"path": str(p), "sha256": sha256_file(p)} for p in (events_path, csv_path, endpoints_path) if p]
    return {
        "kind": "alert_latency",
        "source": "bench",
        "synthetic": synthetic,
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "inputs": inputs,
        "device": device,
        "clock": {"offset_ms": clock_offset_ms, "segment_offsets_ms": {str(k): v for k, v in offsets.items()},
                  "note": "node t_ms mapped per boot onto the capture host's wall clock; gateway and endpoint times "
                          "are compared with it directly, which assumes one machine or a correct --clock-offset-ms"},
        "incidents": len(found),
        "matched": len(used),
        "segments": {name: fxstats.summarize(values) for name, values in seg.items()},
        "per_service": {s: {"delivered": v["delivered"], "crossing_to_delivered_ms": fxstats.summarize(v["_crossing"]),
                            "ack_latency_s": fxstats.summarize(v["_ack"])} for s, v in sorted(per_service.items())},
        "realert": {"configured_s": ack_timeout_s, "observed_s": fxstats.summarize(realert_observed),
                    "early": sum(1 for s in realert_observed if s < ack_timeout_s)},
        "failures": failures,
        "unmatched_deliveries": unmatched,
        "samples": samples_out,
    }


def markdown(report: dict) -> str:
    lines = ["# FireX alert-path latency (bench)", ""]
    if report["synthetic"]:
        lines += ["> **SYNTHETIC DATA.** Generated by the tool's demo, not recorded on a bench. "
                  "Nothing here is a measurement.", ""]
    lines += [f"{report['incidents']} transitions into CRITICAL in the capture, {report['matched']} joined to "
              f"deliveries at the mock endpoints. Generated {report['generated_at']}.", ""]
    names = {"crossing_to_critical_ms": "threshold crossing to CRITICAL (node)",
             "critical_to_gateway_ms": "CRITICAL to stored on the gateway",
             "gateway_to_delivered_ms": "gateway to delivered at the endpoint",
             "crossing_to_delivered_ms": "threshold crossing to delivered (total)"}
    rows = []
    for key, label in names.items():
        s = report["segments"][key]
        rows.append([label, s["n"], fxstats.num(_s(s["median"]), 2), fxstats.num(_s(s["p90"]), 2),
                     fxstats.num(_s(s["max"]), 2)])
    lines += ["## Where the time goes (seconds)", "", fxstats.md_table(["Step", "n", "median", "p90", "max"], rows), ""]
    lines += ["The node's step includes the fusion dwell by design (10 s for two modalities, 3 s for flame alone). "
              "Delivery samples are one per service per incident.", ""]
    rows = []
    for service, v in report["per_service"].items():
        rows.append([service, v["delivered"], fxstats.summary_text({**v["crossing_to_delivered_ms"],
                    **{k: _s(v["crossing_to_delivered_ms"][k]) for k in ("median", "p90", "max")}}, 2, "s"),
                     fxstats.summary_text(v["ack_latency_s"], 1, "s")])
    lines += ["## By service", "", fxstats.md_table(["Service", "Delivered", "Crossing to delivered", "Acknowledged after"], rows), ""]
    r = report["realert"]
    lines += ["## Re-alert timer", "",
              f"Configured {r['configured_s']:.0f} s; observed {fxstats.summary_text(r['observed_s'], 2, 's')}; "
              f"{r['early']} re-alerts sent before the window ended.", ""]
    if report["failures"]:
        lines += ["## Failures", "", fxstats.md_table(["seq", "Why"], [[f["seq"], f["reason"]] for f in report["failures"]]), ""]
    lines += [f"Deliveries not joined to a capture incident: {report['unmatched_deliveries']}.", "",
              f"Clock: {report['clock']['note']} (offset {report['clock']['offset_ms']} ms).", "",
              "## Inputs", ""] + [f"- `{i['path']}` sha256 `{i['sha256'][:16]}…`" for i in report["inputs"]] + [""]
    return "\n".join(lines)


def _s(ms: float | None) -> float | None:
    return None if ms is None else ms / 1000.0


# --- synthetic inputs (NOT measurements) --------------------------------------------

DEMO_DEVICE = "ab" * 16


def demo_files(directory: str | Path, count: int = 12, seed: int = 5, ack_timeout_s: float = 60.0) -> dict[str, Path]:
    """SYNTHETIC: a capture with `count` gas-plus-heat incidents five minutes apart and a
    reboot halfway, and the endpoint log a gateway with four mapped services would have
    produced: fire, ambulance and the site acknowledge, police never does and is re-alerted."""
    rng = random.Random(seed)
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    paths = {"csv": directory / "alerts-demo.csv", "events": directory / "alerts-demo.events.jsonl",
             "endpoints": directory / "alerts-demo.endpoints.jsonl"}
    host0 = 1_790_000_000_000
    boot_host = host0
    events: list[dict] = [{"t_ms": 250, "host_s": 0.0, "host_unix_ms": host0 + 260, "type": "mark", "text": "SYNTHETIC demo"}]
    rows: list[list] = []
    endpoint: list[dict] = []
    seq = 40
    receipt = 0

    def echo(t: int, kind: str, **fields) -> None:
        nonlocal seq
        record = {"t_ms": t, "host_s": round(t / 1000.0, 3), "host_unix_ms": boot_host + t + rng.randint(20, 300), "type": kind}
        if kind != "mark":
            record["seq"] = seq
            seq += 1
        record.update(fields)
        events.append(record)

    t_end = 0
    for i in range(count):
        if i == count // 2:
            boot_host = boot_host + t_end + 4000  # reboot: device time restarts
            echo(300, "boot", fw_version="0.5.0", reset_reason="software", boot_count=9)
            t_end = 0
        start = t_end + 60_000
        for t in range(t_end + 1000, start + 120_000, 1000):
            rows.append([t, "0.1", 1, boot_host + t + rng.randint(5, 40)])
        cross_gas = start + rng.randint(0, 2000)
        cross_heat = cross_gas + rng.randint(1000, 6000)
        echo(cross_gas, "threshold_cross", modality="gas", score=66, direction="up")
        echo(cross_heat, "threshold_cross", modality="heat", score=63, direction="up")
        critical = cross_heat + 10_000
        echo(critical, "state_change", **{"from": "ELEVATED", "to": "CRITICAL", "reason": ["gas", "heat"],
                                          "scores": {"gas": 81, "heat": 64}})
        crit_seq = seq - 1
        stored = boot_host + critical + rng.randint(80, 400)
        incident_id = f"inc-{i + 1}"
        for service, contact in (("site", "warden-1"), ("fire", "station-042"), ("police", "ps-017"), ("ambulance", "ems-north")):
            receipt += 1
            rid = f"r-{receipt:04d}"
            received = stored + rng.randint(30, 250)
            base = {"event": "delivered", "service": service, "recipient": contact, "channel": "api", "receipt_id": rid,
                    "incident_id": incident_id, "node_id": DEMO_DEVICE, "ledger_seq": crit_seq,
                    "ledger_hash": f"{crit_seq:064x}", "ledger_received_at_ms": stored, "retry_of": None, "received_at_ms": received}
            endpoint.append(base)
            if service == "police":
                receipt += 1
                endpoint.append({**base, "receipt_id": f"r-{receipt:04d}", "recipient": "ps-018", "retry_of": rid,
                                 "received_at_ms": received + int(ack_timeout_s * 1000) + rng.randint(50, 1000)})
            else:
                ack_after = {"site": (5, 30), "fire": (20, 45), "ambulance": (15, 50)}[service]
                endpoint.append({"event": "acked", "receipt_id": rid, "service": service, "by": contact,
                                 "at_ms": received + rng.randint(*ack_after) * 1000})
        t_end = start + 120_000

    events.sort(key=lambda e: e["host_unix_ms"])
    with open(paths["events"], "w", encoding="utf-8") as handle:
        handle.writelines(json.dumps(e) + "\n" for e in events)
    with open(paths["csv"], "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["t_ms", "gas", "gas_ok", "host_unix_ms"])
        writer.writerows(sorted(rows, key=lambda r: r[3]))
    endpoint.sort(key=lambda r: r.get("received_at_ms", r.get("at_ms", 0)))
    with open(paths["endpoints"], "w", encoding="utf-8") as handle:
        handle.writelines(json.dumps(r) + "\n" for r in endpoint)
    return paths
