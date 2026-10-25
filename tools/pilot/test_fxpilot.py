import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxpilot  # noqa: E402

DAY = fxpilot.DAY_MS
T0 = 1_790_000_000_000


def alert(service, sent, ack_after_s=None, by=None, timeouts=0, re_alerts=0, recipient="+91••••••0042", drill=False):
    return {"service": service, "channel": "sms", "recipient_masked": recipient, "drill": drill, "sent_at_ms": sent,
            "delivered_at_ms": sent + 800, "acked_at_ms": None if ack_after_s is None else sent + int(ack_after_s * 1000),
            "acked_by": by, "latency_s": ack_after_s, "timeouts": timeouts, "re_alerts": re_alerts, "failed": False}


def site_doc(**overrides):
    doc = {
        "kind": "pilot_site", "synthetic": False, "generated_at": "2026-12-01T00:00:00Z",
        "site": {"id": "site-a", "name": "Hostel"},
        "period": {"from_ms": T0, "to_ms": T0 + 10 * DAY},
        "registry": {"sha256": "ab" * 32, "services": {"site": {"mode": "live"}, "fire": {"mode": "drill", "agreement": None},
                                                       "police": {"mode": "drill", "agreement": None},
                                                       "ambulance": {"mode": "drill", "agreement": None}}},
        "units": [{"id": "a1" * 16, "label": "Room 206", "provisioned_at": T0, "fw_versions": ["0.6.0"], "gaps": [],
                   "self_tests": {"pass": 10, "fail": 0}, "faults_raised": 0, "fw_updates": [], "elevated_episodes": 1}],
        "incidents": [],
        "drills": [{"id": 1, "started_at_ms": T0 + DAY, "finished_at_ms": T0 + DAY + 90_000, "by": "admin",
                    "services": ["site", "fire", "police"],
                    "alerts": [alert("site", T0 + DAY, 12, "warden", drill=True),
                               alert("fire", T0 + DAY, 34, "station-042", drill=True),
                               alert("police", T0 + DAY, None, None, timeouts=2, re_alerts=1, drill=True)]}],
        "site_ledger": {"entries": 10, "verify": "PASS  10 entries", "device_id": "5a" * 16},
        "unit_bundles": [{"unit": "a1" * 16, "file": "u.json", "entries": 50, "verify": "PASS  50 entries"}],
        "traces": [],
    }
    doc.update(overrides)
    return doc


def write_site(root: Path, name: str, doc: dict) -> Path:
    directory = root / name
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "pilot.json").write_text(json.dumps(doc), encoding="utf-8")
    return directory


class AvailabilityTest(unittest.TestCase):
    def test_overlapping_and_open_gaps_are_clipped_to_the_period(self):
        gaps = [{"start_ms": T0 - 3_600_000, "end_ms": T0 + 3_600_000},        # starts before the period: 1 h inside
                {"start_ms": T0 + 1_800_000, "end_ms": T0 + 5_400_000},        # overlaps the first: 30 min more
                {"start_ms": T0 + 10 * DAY - 600_000, "end_ms": None}]          # still open at the end: 10 min
        self.assertEqual(fxpilot.merged_gap_ms(gaps, T0, T0 + 10 * DAY), 3_600_000 + 1_800_000 + 600_000)

    def test_unit_availability_counts_from_provisioning(self):
        unit = {"id": "u", "provisioned_at": T0 + 5 * DAY, "gaps": [{"start_ms": T0 + 6 * DAY, "end_ms": T0 + 6 * DAY + DAY // 2}]}
        s = fxpilot.unit_summary(unit, {"from_ms": T0, "to_ms": T0 + 10 * DAY})
        self.assertAlmostEqual(s["installed_days"], 5.0)
        self.assertAlmostEqual(s["availability"], 0.9)


class IncidentTest(unittest.TestCase):
    def incidents(self):
        base = {"end_ts": None, "reason": ["gas", "smoke"], "escalation_state": "finished", "traces": [], "alerts": []}
        return [
            {**base, "id": 1, "unit": "a1" * 16, "start_ts": (T0 + DAY) // 1000, "ts_src": "gateway", "label": "nuisance",
             "labelled_by": "warden", "labelled_at_ms": T0 + DAY + 600_000, "late": False},
            {**base, "id": 2, "unit": "a1" * 16, "start_ts": (T0 + 2 * DAY) // 1000, "ts_src": "uncertain", "label": "true",
             "labelled_by": "warden", "labelled_at_ms": T0 + 2 * DAY + 60_000, "late": False},
            {**base, "id": 3, "unit": "a1" * 16, "start_ts": (T0 + 3 * DAY) // 1000, "ts_src": "gateway", "label": None,
             "labelled_by": None, "labelled_at_ms": None, "late": True, "escalation_state": "withheld",
             "alerts": [alert("site", T0 + 3 * DAY + 3_000_000, 30, "warden")]},
        ]

    def test_label_completeness_and_time_to_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            site = fxpilot.load_site(write_site(Path(tmp), "a", site_doc(incidents=self.incidents())))
            s = fxpilot.summarize_site(site)
        inc = s["incidents"]
        self.assertEqual(inc["by_label"], {"true": 1, "nuisance": 1, "test": 0, "unlabelled": 1})
        self.assertEqual((inc["labelled"]["k"], inc["labelled"]["n"]), (2, 3))
        # the uncertain clock's incident says nothing about how long labelling took
        self.assertEqual(inc["time_to_label_s"]["n"], 1)
        self.assertEqual(inc["time_to_label_s"]["median"], 600.0)
        self.assertAlmostEqual(inc["false_alarms_per_100_unit_days"]["rate"], 10.0)

    def test_late_incidents_are_listed(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fxpilot.analyse([write_site(Path(tmp), "a", site_doc(incidents=self.incidents()))])
        self.assertEqual([i["id"] for i in report["sites"][0]["incidents"]["late"]], [3])
        self.assertIn("Late: reached the gateway", fxpilot.markdown(report))


class DrillTest(unittest.TestCase):
    def test_a_silent_service_and_its_re_alert_are_on_the_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fxpilot.analyse([write_site(Path(tmp), "a", site_doc())])
        drill = report["sites"][0]["drills"][0]
        police = next(r for r in drill["alerts"] if r["service"] == "police")
        self.assertIsNone(police["latency_s"])
        self.assertEqual((police["timeouts"], police["re_alerts"]), (2, 1))
        self.assertTrue(drill["complete"])
        latency = report["sites"][0]["ack_latency"]["drills"]
        self.assertEqual(latency["police"]["acknowledged"]["k"], 0)
        self.assertEqual(latency["police"]["ack_latency_s"]["n"], 0)
        self.assertEqual(latency["fire"]["ack_latency_s"]["median"], 34.0)
        self.assertIn("no acknowledgement", fxpilot.markdown(report))

    def test_a_drill_with_nobody_acknowledging_is_not_a_complete_record(self):
        rows = [fxpilot.alert_row(alert("site", T0, None, None, timeouts=1), "drill")]
        self.assertFalse(fxpilot.record_complete(rows))


class CriteriaTest(unittest.TestCase):
    def test_synthetic_only_is_no_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = write_site(Path(tmp), "a", site_doc(synthetic=True))
            b = write_site(Path(tmp), "b", site_doc(synthetic=True, site={"id": "site-b", "name": "Ward"}))
            report = fxpilot.analyse([a, b])
        self.assertTrue(all(c["status"] == "no data (synthetic only)" for c in report["criteria"]))
        self.assertTrue(report["combined"]["synthetic"])

    def test_one_real_site_is_not_the_milestone(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = fxpilot.analyse([write_site(Path(tmp), "a", site_doc())])
        status = {c["name"]: c["status"] for c in report["criteria"]}
        self.assertEqual(status["Two live sites with data over the period"], "not met")
        self.assertEqual(report["criteria"][-1]["status"], "not met")
        self.assertEqual(status["Site ledger and unit bundles verify"], "met")

    def test_two_real_sites_with_drills_meet_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = write_site(Path(tmp), "a", site_doc())
            b = write_site(Path(tmp), "b", site_doc(site={"id": "site-b", "name": "Ward"}))
            report = fxpilot.analyse([a, b])
        self.assertEqual([c["status"] for c in report["criteria"]], ["met"] * len(report["criteria"]))

    def test_a_tampered_record_is_flagged_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc = site_doc(site_ledger={"entries": 10, "verify": "TAMPERED at entry 4 (chain): hash does not match", "device_id": "5a" * 16})
            report = fxpilot.analyse([write_site(Path(tmp), "a", doc)])
        text = fxpilot.markdown(report)
        self.assertLess(text.index("Records that do not verify"), text.index("## M5 exit criteria"))
        self.assertEqual(report["criteria"][3]["status"], "not met")


class PrivacyAndBannerTest(unittest.TestCase):
    def test_an_unmasked_number_is_masked_anyway(self):
        doc = site_doc()
        doc["drills"][0]["alerts"][1]["recipient_masked"] = "+919876543210"
        with tempfile.TemporaryDirectory() as tmp:
            report = fxpilot.analyse([write_site(Path(tmp), "a", doc)])
        text = fxpilot.markdown(report)
        self.assertNotIn("9876543210", text)
        self.assertNotIn("98765", json.dumps(report, ensure_ascii=False))
        self.assertIn("3210", text)
        self.assertEqual(fxpilot.mask_recipient("station-042"), "station-042")
        self.assertEqual(fxpilot.mask_recipient("+91••••••4321"), "+91••••••4321")
        self.assertEqual(fxpilot.mask_recipient("+91••9876543210"), "+••••••••••3210")

    def test_banner_only_when_an_input_is_synthetic(self):
        with tempfile.TemporaryDirectory() as tmp:
            real = fxpilot.markdown(fxpilot.analyse([write_site(Path(tmp), "a", site_doc())]))
            fake = fxpilot.markdown(fxpilot.analyse([write_site(Path(tmp), "b", site_doc(synthetic=True))]))
        self.assertNotIn("SYNTHETIC INPUT PRESENT", real)
        self.assertIn("SYNTHETIC INPUT PRESENT", fake)
        self.assertIn("Simulated, not measured", fake)


class DemoTest(unittest.TestCase):
    def test_the_demo_writes_two_synthetic_sites_and_reports_on_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = fxpilot.write_demo(tmp)
            report = fxpilot.analyse(paths)
        self.assertEqual(len(report["sites"]), 2)
        self.assertTrue(report["synthetic"])
        self.assertEqual(report["criteria"][-1]["status"], "no data (synthetic only)")
        self.assertTrue(any(u["fw_updates"]["rolled_back"] for s in report["sites"] for u in s["units"]))
        self.assertIn("SYNTHETIC", fxpilot.markdown(report))


if __name__ == "__main__":
    unittest.main()
