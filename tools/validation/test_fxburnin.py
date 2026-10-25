import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxburnin  # noqa: E402

HOUR = 3_600_000


def criterion(report: dict, name: str) -> dict:
    return next(c for c in report["criteria"] if c["name"] == name)


def healthy_run(out: Path, hours: int = 2, **inject) -> fxburnin.SyntheticRun:
    """A boot, then an injection every 30 minutes and a status snapshot every 10."""
    run = fxburnin.SyntheticRun(out, period_ms=5000)
    run.boot("power_on")
    run.advance(600_000)
    for minute in range(hours * 60):
        if minute % 10 == 0:
            run.snapshot()
        if minute % 30 == 15:
            run.inject(**inject)
        run.advance(60_000 - (run.host - run.boot_host) % 60_000)
    return run


class HealthyRunTest(unittest.TestCase):
    def test_a_clean_run_passes_every_criterion(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out)
            run.close()
            report = fxburnin.analyse(out, bundle_path=run.paths["ledger"])
            failed = [c for c in report["criteria"] if not c["ok"]]
            self.assertEqual(failed, [])
            self.assertTrue(report["pass"])
            self.assertTrue(report["synthetic"])
            self.assertEqual(report["injections"]["sent"], 4)
            self.assertEqual(report["injections"]["detected"]["k"], 4)
            self.assertAlmostEqual(report["injections"]["tta_s"]["median"], 4.5, delta=0.01)
            self.assertEqual(report["cross_check"]["missing_from_serial"], 0)
            self.assertIn("PASS", fxburnin.markdown(report))


class FailureTest(unittest.TestCase):
    def test_a_refused_injection_is_a_miss_with_its_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1)
            run.inject(refuse="the node is not in NORMAL")
            run.close()
            report = fxburnin.analyse(out)
            self.assertEqual(report["injections"]["sent"], 3)
            self.assertEqual(report["injections"]["missed"][0]["reason"], "refused: the node is not in NORMAL")
            self.assertFalse(criterion(report, "every injection reached CRITICAL in time")["ok"])
            self.assertFalse(report["pass"])

    def test_an_injection_refused_for_the_self_test_is_retried_not_missed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1)
            run.inject(refuse="a self-test is running")
            run.advance(60_000)
            run.schedule.write(json.dumps({"host_unix_ms": run.host, "event": "inject_retry"}) + "\n")
            run.inject()
            run.advance(60_000)
            run.close()
            report = fxburnin.analyse(out)
            self.assertEqual(report["injections"]["sent"], 3)
            self.assertEqual(report["injections"]["detected"]["k"], 3)
            self.assertEqual(report["injections"]["missed"], [])
            self.assertEqual(len(report["injections"]["deferred"]), 1)
            self.assertTrue(criterion(report, "every injection reached CRITICAL in time")["ok"])

    def test_an_injection_without_critical_is_missed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1)
            run.inject(critical_after_ms=None)
            run.close()
            report = fxburnin.analyse(out)
            self.assertEqual([m["reason"] for m in report["injections"]["missed"]], ["no CRITICAL within 15 s"])

    def test_a_slow_critical_is_missed_under_a_tight_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1, critical_after_ms=7000, seconds=10)
            run.close()
            self.assertTrue(fxburnin.analyse(out, tta_limit_s=15)["injections"]["missed"] == [])
            self.assertEqual(len(fxburnin.analyse(out, tta_limit_s=5)["injections"]["missed"]), 2)

    def test_a_watchdog_reboot_mid_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1)
            run.advance(10_000, stream=False)
            run.boot("watchdog")
            run.advance(120_000)
            run.inject(critical_after_ms=5200)  # device time restarted: TTA must still be 5.2 s
            run.advance(120_000)
            run.close()
            report = fxburnin.analyse(out, bundle_path=run.paths["ledger"])
            self.assertEqual(report["resets"]["watchdog"], 1)
            self.assertEqual(report["boots"], 2)
            self.assertFalse(criterion(report, "no watchdog, panic or brownout reset")["ok"])
            self.assertAlmostEqual(report["injections"]["tta_s"]["max"], 5.2, delta=0.01)
            self.assertEqual(report["injections"]["detected"]["k"], 3)

    def test_a_critical_nobody_injected(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1)
            run.event("state_change", **{"from": "NORMAL", "to": "CRITICAL", "reason": ["gas", "smoke"], "scores": {}})
            run.close()
            report = fxburnin.analyse(out)
            self.assertEqual(len(report["unexpected_critical"]), 1)
            self.assertFalse(criterion(report, "no CRITICAL without an injection")["ok"])

    def test_the_bundle_counts_a_fault_serial_missed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out)
            run.event("fault", echo=False, source="heat", state="raised")
            run.event("log_overwrite", first_seq=0, last_seq=40, mirrored=True)
            run.close()
            serial_only = fxburnin.analyse(out)
            self.assertEqual(serial_only["faults_raised"], 0)
            with_bundle = fxburnin.analyse(out, bundle_path=run.paths["ledger"])
            self.assertEqual(with_bundle["faults_raised"], 1)
            self.assertEqual(with_bundle["fault_sources"], {"heat": 1})
            self.assertEqual(with_bundle["cross_check"]["missing_from_serial"], 1)
            self.assertEqual(with_bundle["log_overwrite"], {"mirrored": 1, "unmirrored": 0})
            self.assertFalse(criterion(with_bundle, "no fault raised")["ok"])
            self.assertTrue(criterion(with_bundle, "nothing overwritten before the gateway had it")["ok"])

    def test_falling_heap_and_a_flagged_buzzer(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = fxburnin.SyntheticRun(out, period_ms=5000)
            run.boot()
            for i in range(20):
                run.advance(600_000)
                run.heap = 180_000 - i * 2000  # 12 kB an hour
                run.snapshot(faults={"sensor": False, "ledger": False, "selftest": False, "buzzer": i == 5}, dropped=i // 10)
            run.close()
            report = fxburnin.analyse(out)
            self.assertAlmostEqual(report["heap"]["slope_bytes_per_h"], -12_000, delta=1)
            self.assertFalse(criterion(report, "free heap steady")["ok"])
            self.assertFalse(criterion(report, "status never showed a fault flag")["ok"])
            self.assertFalse(criterion(report, "no ledger write error or dropped event")["ok"])

    def test_a_stream_gap_is_incomplete_without_an_export(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "burnin"
            run = healthy_run(out, hours=1)
            run.advance(300_000, stream=False)
            run.advance(60_000)
            run.close()
            self.assertFalse(criterion(fxburnin.analyse(out), "record complete")["ok"])
            gaps = fxburnin.analyse(out)["stream_gaps"]
            self.assertEqual(len(gaps), 1)
            self.assertAlmostEqual(gaps[0]["seconds"], 305.0, delta=5)
            self.assertTrue(criterion(fxburnin.analyse(out, bundle_path=run.paths["ledger"]), "record complete")["ok"])


class ParsingTest(unittest.TestCase):
    def test_status_reply_with_stream_lines_between(self):
        c = fxburnin.StatusCollector()
        lines = [
            "FireX node 0.5.0+inject, boot 12 (power_on), uptime 3605 s, free heap 181234",
            "S,3605000,0.1,1",
            "fusion NORMAL, alarm NORMAL",
            "faults: sensor no, ledger no, self-test no, buzzer YES",
            "device_id 3a91c4",
            "ledger open, next seq 812, appended this boot 44, failures 0",
            "segments 0-3, removed this boot 0, partition 120 of 1024 KiB used",
        ]
        for line in lines:
            self.assertIsNone(c.feed(line, 1000))
        snap = c.feed("queue dropped 2, time source uncertain, clock base 1789000000", 1001)
        self.assertEqual(snap["boot"], 12)
        self.assertEqual(snap["free_heap"], 181234)
        self.assertEqual(snap["uptime_s"], 3605)
        self.assertTrue(snap["faults"]["buzzer"])
        self.assertEqual((snap["next_seq"], snap["failures"], snap["queue_dropped"]), (812, 0, 2))

    def test_the_boot_banner_is_not_a_status_reply(self):
        c = fxburnin.StatusCollector()
        c.feed("FireX node 0.5.0, boot 3 (watchdog)", 5)
        self.assertIsNone(c.current)

    def test_refusal(self):
        self.assertEqual(fxburnin.refusal("firex> inject refused: a self-test is running"), "a self-test is running")
        self.assertIsNone(fxburnin.refusal("inject flame 8"))

    def test_old_captures_without_host_time_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "old"
            fxburnin.run_paths(out)["csv"].write_text("t_ms,gas,host_s\n1000,0.1,0.5\n", encoding="utf-8")
            fxburnin.run_paths(out)["events"].write_text(json.dumps({"t_ms": 1, "type": "mark"}) + "\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                fxburnin.analyse(out)


class DemoTest(unittest.TestCase):
    def test_the_demo_is_marked_synthetic_and_shows_its_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "demo"
            bundle = fxburnin.demo_run(out, hours=3)
            report = fxburnin.analyse(out, bundle_path=bundle, synthetic=True)
            self.assertTrue(report["synthetic"])
            self.assertIn("SYNTHETIC", fxburnin.markdown(report))
            self.assertEqual(report["injections"]["sent"], 6)
            self.assertEqual(len(report["injections"]["missed"]), 1)


if __name__ == "__main__":
    unittest.main()
