import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxlatency  # noqa: E402

DEVICE = "cd" * 16


class Bench:
    """Hand-built capture and endpoint log with exact, known times."""

    def __init__(self, directory: Path):
        self.dir = directory
        self.events: list[dict] = []
        self.rows: list[tuple[int, int]] = []
        self.endpoint: list[dict] = []
        self.boot_host = 1_790_000_000_000
        self.receipts = 0

    def stream(self, t_from: int, t_to: int, delay: int = 12) -> None:
        for t in range(t_from, t_to, 1000):
            self.rows.append((self.boot_host + t + delay, t))

    def echo(self, t: int, kind: str, delay: int = 150, **fields) -> None:
        self.events.append({"t_ms": t, "host_unix_ms": self.boot_host + t + delay, "type": kind, **fields})

    def deliver(self, seq: int, incident: str, service: str, stored: int, received: int, retry_of: str | None = None,
                ack_at: int | None = None, node: str = DEVICE) -> str:
        self.receipts += 1
        rid = f"r-{self.receipts:04d}"
        self.endpoint.append({"event": "delivered", "service": service, "recipient": service, "channel": "api",
                              "receipt_id": rid, "incident_id": incident, "node_id": node, "ledger_seq": seq,
                              "ledger_hash": "00", "ledger_received_at_ms": stored, "retry_of": retry_of,
                              "received_at_ms": received})
        if ack_at is not None:
            self.endpoint.append({"event": "acked", "receipt_id": rid, "service": service, "by": service, "at_ms": ack_at})
        return rid

    def write(self) -> tuple[Path, Path, Path]:
        events, csv_path, endpoints = self.dir / "cap.events.jsonl", self.dir / "cap.csv", self.dir / "endpoints.jsonl"
        events.write_text("".join(json.dumps(e) + "\n" for e in self.events), encoding="utf-8")
        with open(csv_path, "w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(["t_ms", "gas", "host_s", "host_unix_ms"])
            for host, t in self.rows:
                writer.writerow([t, "0.1", 0, host])
        endpoints.write_text("".join(json.dumps(r) + "\n" for r in self.endpoint), encoding="utf-8")
        return events, csv_path, endpoints


class ClockTest(unittest.TestCase):
    def test_a_lagging_echo_is_not_a_reboot_but_a_boot_is(self):
        points = [(1000, 50_000, False), (1100, 50_100, False), (1200, 47_000, False),  # echo queued 3 s earlier
                  (9000, 300, True), (9100, 400, False)]
        self.assertEqual(fxlatency.boot_segments(points), [0, 0, 0, 1, 1])

    def test_a_reboot_whose_boot_echo_was_lost(self):
        points = [(1000, 900_000, False), (5000, 2_000, False), (6000, 3_000, False)]
        self.assertEqual(fxlatency.boot_segments(points), [0, 1, 1])

    def test_offsets_per_boot_are_the_smallest_serial_delay(self):
        events = [{"t_ms": 5_000, "host_unix_ms": 1_000_000 + 5_000 + 400, "type": "threshold_cross"},
                  {"t_ms": 300, "host_unix_ms": 2_000_000 + 300 + 90, "type": "boot"}]
        samples = [(1_000_000 + t + 15, t) for t in range(1000, 9000, 1000)]
        samples += [(2_000_000 + t + 8, t) for t in range(1000, 9000, 1000)]
        mapped, offsets = fxlatency.map_device_time(events, samples)
        self.assertEqual(offsets, {0: 1_000_015, 1: 2_000_008})
        self.assertEqual(mapped[0]["device_host_ms"], 1_000_015 + 5_000)
        self.assertEqual(mapped[1]["segment"], 1)


class JoinTest(unittest.TestCase):
    def build(self, tmp: Path) -> Bench:
        b = Bench(tmp)
        # boot 1: gas crosses at 60 s, heat at 64 s, CRITICAL at 74 s (seq 10)
        b.stream(1000, 90_000)
        b.echo(60_000, "threshold_cross", seq=8, modality="gas", direction="up", score=66)
        b.echo(62_000, "threshold_cross", seq=9, modality="smoke", direction="up", score=61)  # not a reason
        b.echo(64_000, "threshold_cross", seq=10, modality="heat", direction="up", score=63)
        b.echo(74_000, "state_change", seq=11, **{"from": "ELEVATED", "to": "CRITICAL", "reason": ["gas", "heat"]})
        crit1 = b.boot_host + 74_000 + 12  # the node's CRITICAL on the host clock
        b.deliver(11, "inc-1", "fire", stored=crit1 + 300, received=crit1 + 450, ack_at=crit1 + 450 + 31_000)
        police = b.deliver(11, "inc-1", "police", stored=crit1 + 300, received=crit1 + 500)
        b.deliver(11, "inc-1", "police", stored=crit1 + 300, received=crit1 + 500 + 60_400, retry_of=police)
        # reboot: device time restarts; flame alone 3 s after its crossing (seq 30)
        b.boot_host += 200_000
        b.echo(300, "boot", seq=20, reset_reason="software")
        b.stream(1000, 40_000, delay=20)
        b.echo(20_000, "threshold_cross", seq=29, modality="flame", direction="up", score=95)
        b.echo(23_000, "state_change", seq=30, **{"from": "NORMAL", "to": "CRITICAL", "reason": ["flame"]})
        crit2 = b.boot_host + 23_000 + 20
        b.deliver(30, "inc-2", "fire", stored=crit2 + 250, received=crit2 + 390)
        # a third CRITICAL the gateway never alerted on
        b.echo(33_000, "state_change", seq=31, **{"from": "NORMAL", "to": "CRITICAL", "reason": ["flame"]})
        # another unit's delivery, ignored with --device
        b.deliver(30, "inc-9", "fire", stored=crit2, received=crit2 + 10, node="ef" * 16)
        return b

    def test_steps_add_up_across_a_reboot(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            report = fxlatency.analyse(*b.write(), device=DEVICE, ack_timeout_s=60)
            self.assertEqual((report["incidents"], report["matched"]), (3, 2))
            first, second = report["samples"][0], report["samples"][1]
            self.assertEqual(first["crossing_to_critical_ms"], 14_000)  # from gas, the earliest reason crossing
            self.assertEqual(first["crossing_modality"], "gas")
            self.assertEqual(first["critical_to_gateway_ms"], 300)
            self.assertEqual(first["deliveries"]["fire"]["gateway_to_delivered_ms"], 150)
            self.assertEqual(first["deliveries"]["fire"]["crossing_to_delivered_ms"], 14_450)
            self.assertEqual(first["deliveries"]["fire"]["ack_latency_s"], 31.0)
            self.assertEqual(second["crossing_to_critical_ms"], 3_000)
            self.assertEqual(second["deliveries"]["fire"]["crossing_to_delivered_ms"], 3_390)
            self.assertEqual(report["segments"]["crossing_to_delivered_ms"]["n"], 3)
            self.assertEqual(report["realert"]["observed_s"]["n"], 1)
            self.assertAlmostEqual(report["realert"]["observed_s"]["max"], 60.4)
            self.assertEqual(report["realert"]["early"], 0)

    def test_failures_and_strangers(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            report = fxlatency.analyse(*b.write(), device=DEVICE)
            reasons = [(f["seq"], f["reason"]) for f in report["failures"]]
            self.assertIn((30, "no delivery to police"), reasons)
            self.assertIn((31, "no delivery recorded"), reasons)
            self.assertEqual(report["unmatched_deliveries"], 0)
            everyone = fxlatency.analyse(*b.write())
            self.assertEqual(everyone["unmatched_deliveries"], 1)

    def test_an_early_realert_is_counted(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            report = fxlatency.analyse(*b.write(), device=DEVICE, ack_timeout_s=61)
            self.assertEqual(report["realert"]["early"], 1)

    def test_a_reused_seq_joins_the_nearest_incident(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = Bench(Path(tmp))
            b.stream(1000, 30_000)
            b.echo(10_000, "state_change", seq=5, **{"to": "CRITICAL", "reason": ["flame"]})
            b.boot_host += 3_600_000  # the ledger was wiped and the unit re-provisioned: seq 5 again
            b.echo(300, "boot", seq=0, reset_reason="power_on")
            b.stream(1000, 30_000)
            b.echo(10_000, "state_change", seq=5, **{"to": "CRITICAL", "reason": ["flame"]})
            late = b.boot_host + 10_000
            b.deliver(5, "inc-b", "fire", stored=late + 200, received=late + 300)
            b.deliver(5, "inc-a", "fire", stored=late - 3_600_000 + 200, received=late - 3_600_000 + 300)
            report = fxlatency.analyse(*b.write())
            self.assertEqual([s["incident_id"] for s in report["samples"]], ["inc-a", "inc-b"])
            self.assertEqual([s["critical_to_gateway_ms"] for s in report["samples"]], [188, 188])


class DemoTest(unittest.TestCase):
    def test_demo_inputs_are_synthetic_and_join(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = fxlatency.demo_files(tmp, count=6)
            report = fxlatency.analyse(paths["events"], paths["csv"], paths["endpoints"], device=fxlatency.DEMO_DEVICE)
            self.assertTrue(report["synthetic"])
            self.assertEqual((report["incidents"], report["matched"]), (6, 6))
            self.assertEqual(report["failures"], [])
            self.assertEqual(report["realert"]["observed_s"]["n"], 6)
            self.assertEqual(report["realert"]["early"], 0)
            self.assertIn("SYNTHETIC", fxlatency.markdown(report))


if __name__ == "__main__":
    unittest.main()
