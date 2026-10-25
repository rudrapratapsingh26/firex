import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxstats  # noqa: E402
import report  # noqa: E402


def replay_doc(synthetic: bool, categories: list[str], hazards: list[str], configs: int = 1) -> dict:
    trials = [{"capture": "c.csv", "class": "nuisance", "category": c, "name": f"{c}-1", "outcome": "quiet",
               "max_state": "NORMAL", "tta_s": None} for c in categories]
    trials += [{"capture": "c.csv", "class": "hazard", "category": h, "name": f"{h}-1", "outcome": "detected",
                "max_state": "CRITICAL", "tta_s": 12.0} for h in hazards]
    config = {
        "name": "node defaults", "settings": {},
        "hazard": {"detection": fxstats.rate(len(hazards), len(hazards)), "miss": fxstats.rate(0, len(hazards)),
                   "tta_s": fxstats.summarize([12.0] * len(hazards)), "by_category": {}},
        "nuisance": {"false_alarm": fxstats.rate(0, len(categories)), "elevated": fxstats.rate(0, len(categories)),
                     "by_category": {c: {"false_alarm": fxstats.rate(0, 1), "elevated": fxstats.rate(0, 1)} for c in categories}},
        "background": {"hours": 2.0, "false_alarms": 0, "per_24h": 0.0},
        "fidelity": {"node_transitions": 4, "matched": 4, "replay_transitions": 4, "config_changes_in_capture": 0},
        "trials": trials,
    }
    return {"kind": "replay", "synthetic": synthetic, "engine": {"binary": "fusion_cli", "sha256": "ab"}, "grace_s": 60,
            "captures": [{"path": "c.csv", "sha256": "cd" * 32, "samples": 100, "duration_s": 100, "period_ms": 500,
                          "gaps": 0, "boots": 1, "trials": len(trials)}],
            "configs": [dict(config, name=f"config {i}") for i in range(configs)], "warnings": []}


def burnin_doc(hours: float, faults: int = 0, watchdog: int = 0) -> dict:
    return {"kind": "burnin", "synthetic": False, "hours": hours,
            "injections": {"sent": 144, "detected": fxstats.rate(144, 144), "tta_s": fxstats.summarize([5.0, 5.5]), "missed": []},
            "faults_raised": faults, "fault_sources": {"heat": faults} if faults else {},
            "resets": {"watchdog": watchdog, "panic": 0, "brownout": 0, "other": 0}, "boots": 1 + watchdog,
            "self_tests": {"pass": 3, "fail": 0}, "ledger_errors": 0, "log_overwrite": {"mirrored": 2, "unmirrored": 0},
            "heap": {"first": 120000, "last": 119000, "min": 118000, "slope_bytes_per_h": -13.9},
            "criteria": [{"name": "no watchdog resets", "ok": watchdog == 0, "detail": f"{watchdog}"}], "pass": faults == 0 and watchdog == 0}


def latency_doc(source: str, synthetic: bool = False, early: int = 0) -> dict:
    s = fxstats.summarize([120.0, 180.0, 240.0])
    return {"kind": "alert_latency", "source": source, "synthetic": synthetic, "incidents": 3,
            "segments": {"crossing_to_critical_ms": s, "critical_to_gateway_ms": s, "gateway_to_delivered_ms": s,
                         "crossing_to_delivered_ms": s},
            "per_service": {"fire": {"delivered": 3, "crossing_to_delivered_ms": s, "ack_latency_s": fxstats.summarize([30.0])}},
            "realert": {"configured_s": 60, "observed_s": fxstats.summarize([60.2, 60.9]), "early": early}, "failures": 0}


def inputs(*docs: dict) -> list[dict]:
    return [{"path": f"in{i}.json", "sha256": f"{i:064x}", "doc": d} for i, d in enumerate(docs)]


def criteria(rendered_inputs: list[dict]) -> dict:
    return {row[0].split(":")[0]: row[1] for row in report.exit_criteria(rendered_inputs)}


class SyntheticTest(unittest.TestCase):
    def test_synthetic_input_is_labelled_and_never_meets_a_criterion(self):
        docs = inputs(replay_doc(True, report.NUISANCE_MATRIX, ["gas", "smoke", "flame"], configs=2),
                      latency_doc("dry_run", synthetic=True))
        text = report.render(docs, "2026-09-15T00:00:00+00:00")
        self.assertIn("SYNTHETIC INPUT PRESENT", text)
        self.assertIn("Simulated, not measured", text)
        self.assertNotIn("| met |", text)
        self.assertIn("no data (synthetic only)", text)

    def test_no_inputs_at_all(self):
        text = report.render([], "2026-09-15T00:00:00+00:00")
        self.assertNotIn("SYNTHETIC", text)
        self.assertIn("No data.", text)


class CriteriaTest(unittest.TestCase):
    def test_everything_measured(self):
        rows = criteria(inputs(replay_doc(False, report.NUISANCE_MATRIX, ["gas", "smoulder", "flame"], configs=3),
                               burnin_doc(72.5), latency_doc("dry_run"), latency_doc("bench"),
                               {"kind": "camera", "synthetic": False, "reference": {}, "frames": [{"file": "a.jpg"}]}))
        self.assertTrue(all(status == "met" for status in rows.values()), rows)

    def test_gaps_are_named(self):
        docs = inputs(replay_doc(False, ["cooking", "steam"], ["gas"]), burnin_doc(40.0, faults=2, watchdog=1),
                      latency_doc("dry_run", early=1))
        table = report.exit_criteria(docs)
        by_name = {row[0].split(":")[0]: row for row in table}
        self.assertEqual(by_name["Nuisance matrix"][1], "not met")
        self.assertIn("incense", by_name["Nuisance matrix"][2])
        self.assertIn("smoke", by_name["Real gas, smoke and flame exposures"][2])
        self.assertEqual(by_name["72-hour burn-in with scheduled injections"][1], "not met")
        self.assertEqual(by_name["Zero FAULT entries and zero watchdog resets"][1], "not met")
        self.assertEqual(by_name["Escalation dry runs against mock endpoints"][1], "not met")
        self.assertEqual(by_name["Alert-path latency distribution, threshold crossing to alert delivered"][1], "no data")
        self.assertEqual(by_name["Rules tuned against the recorded trials"][1], "not met")


class RenderTest(unittest.TestCase):
    def test_sections_and_tolerance_of_sparse_documents(self):
        docs = inputs(replay_doc(False, ["aerosol"], [], configs=2), burnin_doc(72.0),
                      latency_doc("bench"), latency_doc("dry_run"),
                      {"kind": "alert_latency", "source": "dry_run"},  # nearly empty: must not crash
                      {"kind": "camera"}, {"kind": "mystery"})
        text = report.render(docs, "2026-09-15T00:00:00+00:00")
        self.assertIn("Configurations compared", text)
        self.assertIn("Escalation dry run against mock endpoints", text)
        self.assertLess(text.index("Escalation dry run"), text.index("Alert path on the bench"))
        self.assertIn("## Not understood", text)
        self.assertIn("threshold crossing to alert delivered (end to end)", text)

    def test_load_inputs_hashes_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "r.json"
            path.write_text(json.dumps(burnin_doc(1.0)), encoding="utf-8")
            loaded = report.load_inputs([str(path)])
            self.assertEqual(len(loaded[0]["sha256"]), 64)
            bad = Path(tmp) / "bad.json"
            bad.write_text("[]", encoding="utf-8")
            with self.assertRaises(ValueError):
                report.load_inputs([str(bad)])


if __name__ == "__main__":
    unittest.main()
