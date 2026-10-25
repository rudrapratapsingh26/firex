import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fxreplay  # noqa: E402
from fxengine import DEFAULT_SETTINGS, available, check_settings, config_line  # noqa: E402

COLUMNS = ["t_ms", "mq2_rs", "gas", "gas_ok", "smoke", "smoke_ok", "flame", "flame_ok", "heat", "heat_ok",
           "rh", "rh_ok", "fan_duty", "fan_rpm", "fan_state", "alarm", "host_s", "host_unix_ms"]


def write_capture(directory: Path, name: str, rows: list[dict], events: list[dict]) -> Path:
    path = directory / f"{name}.csv"
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({c: row.get(c, "") for c in COLUMNS})
    (directory / f"{name}.events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")
    return path


def row(t_ms: int, host_s: float, **scores) -> dict:
    base = {"t_ms": t_ms, "mq2_rs": 9000, "fan_duty": 100, "fan_rpm": 1450, "fan_state": "on", "alarm": "NORMAL",
            "host_s": host_s}
    for m in ("gas", "smoke", "flame", "heat", "rh"):
        base[m] = f"{scores.get(m, 0.05):.3f}"
        base[f"{m}_ok"] = 1
    return base


def mark(t_ms: int, host_s: float, text: str) -> dict:
    return {"t_ms": t_ms, "host_s": host_s, "type": "mark", "text": text}


def trial(cls: str, start_ms: int, end_ms: int, name: str = "x") -> fxreplay.Trial:
    return fxreplay.Trial("cap.csv", 0, cls, name, start_ms, end_ms)


class CaptureLoadingTest(unittest.TestCase):
    def test_marks_become_trials_with_categories(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = [row(10_000 + 1000 * i, float(i)) for i in range(60)]
            events = [mark(15_000, 5.0, "trial nuisance aerosol-deodorant"), mark(30_000, 20.0, "trial end"),
                      mark(40_000, 30.0, "trial hazard smoulder-bin"), mark(50_000, 40.0, "trial end"),
                      mark(52_000, 42.0, "trial nuisance welding-arc")]
            cap = fxreplay.load_capture(write_capture(Path(tmp), "run", rows, events))
        self.assertEqual([(t.cls, t.category, t.start_ms, t.end_ms, t.closed) for t in cap.trials], [
            ("nuisance", "aerosol", 15_000, 30_000, True),
            ("hazard", "smoulder", 40_000, 50_000, True),
            ("nuisance", "other", 52_000, 69_000, False),  # never ended: closed at the last sample
        ])
        self.assertEqual(cap.period_ms, 1000)
        self.assertFalse(cap.synthetic)
        self.assertTrue(any("not ended" in w for w in cap.warnings))

    def test_a_reboot_splits_the_capture_and_events_follow_their_boot(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = [row(500_000 + 1000 * i, float(i)) for i in range(10)]
            second = [row(20_000 + 1000 * i, 100.0 + i) for i in range(10)]  # booted at host 80 s
            events = [mark(505_000, 5.2, "trial hazard gas-lpg"), mark(508_000, 8.1, "trial end"),
                      {"t_ms": 1200, "host_s": 81.3, "type": "boot", "reset_reason": "watchdog"},
                      mark(22_000, 102.1, "trial nuisance steam-kettle"), mark(25_000, 105.0, "trial end")]
            cap = fxreplay.load_capture(write_capture(Path(tmp), "rb", first + second, events))
        self.assertEqual(len(cap.segments), 2)
        self.assertEqual([t.segment for t in cap.trials], [0, 1])
        self.assertEqual(cap.segments[1].events[0]["type"], "boot")

    def test_without_host_times_events_follow_clock_restarts(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = [row(90_000 + 1000 * i, 0.0) for i in range(10)] + [row(3000 + 1000 * i, 0.0) for i in range(10)]
            for r in rows:
                r["host_s"] = ""  # an older capture: no host clock at all
            events = [{"t_ms": 92_000, "type": "mark", "text": "trial hazard flame-candle"},
                      {"t_ms": 95_000, "type": "mark", "text": "trial end"},
                      {"t_ms": 1000, "type": "boot"},
                      {"t_ms": 5000, "type": "state_change", "to": "NORMAL"}]
            cap = fxreplay.load_capture(write_capture(Path(tmp), "old", rows, events))
        self.assertEqual([len(s.events) for s in cap.segments], [2, 2])
        self.assertEqual(cap.trials[0].segment, 0)

    def test_gaps_and_invalid_cells(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = [row(1000 * i, float(i)) for i in range(5)] + [row(60_000, 60.0)]
            rows[2]["gas_ok"] = 0
            rows[3]["smoke"] = ""
            cap = fxreplay.load_capture(write_capture(Path(tmp), "g", rows, []))
        samples = cap.segments[0].samples
        self.assertEqual(cap.gaps, 1)
        self.assertFalse(samples[2].valid[0])
        self.assertEqual((samples[3].scores[1], samples[3].valid[1]), (0.0, False))

    def test_labels_replace_marks(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = [row(1000 * i, float(i)) for i in range(100)]
            path = write_capture(Path(tmp), "lab", rows, [mark(5000, 5.0, "trial hazard gas-a"), mark(9000, 9.0, "trial end")])
            labels_path = Path(tmp) / "trials.csv"
            labels_path.write_text("capture,class,name,start_s,end_s\nlab.csv,nuisance,dust-broom,20,40\n", encoding="utf-8")
            labels = fxreplay.load_labels(labels_path)
            cap = fxreplay.load_capture(path, labels=fxreplay.labels_for(labels, path))
        self.assertEqual([(t.cls, t.name, t.start_ms, t.end_ms) for t in cap.trials], [("nuisance", "dust-broom", 20_000, 40_000)])

    def test_not_a_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.csv"
            path.write_text("a,b\n1,2\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                fxreplay.load_capture(path)


class OutcomeTest(unittest.TestCase):
    timeline = [(0, "NORMAL"), (10_000, "ELEVATED"), (20_000, "CRITICAL"), (30_000, "NORMAL"), (200_000, "NORMAL")]

    def test_hazard_detected_with_time_to_alarm(self):
        result = fxreplay.evaluate_trial(trial("hazard", 5_000, 25_000), self.timeline, 60_000)
        self.assertEqual((result["outcome"], result["tta_s"], result["max_state"]), ("detected", 15.0, "CRITICAL"))
        self.assertEqual(result["elevated_at_s"], 10.0)

    def test_critical_inside_the_grace_counts(self):
        result = fxreplay.evaluate_trial(trial("hazard", 0, 15_000), self.timeline, 5_000)
        self.assertEqual(result["outcome"], "detected")
        result = fxreplay.evaluate_trial(trial("hazard", 0, 10_000), self.timeline, 5_000)
        self.assertEqual((result["outcome"], result["tta_s"]), ("missed", None))

    def test_nuisance_outcomes(self):
        self.assertEqual(fxreplay.evaluate_trial(trial("nuisance", 0, 25_000), self.timeline, 0)["outcome"], "false_alarm")
        self.assertEqual(fxreplay.evaluate_trial(trial("nuisance", 5_000, 15_000), self.timeline, 0)["outcome"], "elevated")
        self.assertEqual(fxreplay.evaluate_trial(trial("nuisance", 100_000, 150_000), self.timeline, 0)["outcome"], "quiet")

    def test_background_time_and_false_alarms(self):
        timeline = [(t * 1000, "CRITICAL" if 50 <= t < 55 or 80 <= t < 85 else "NORMAL") for t in range(0, 100)]
        ms, false_alarms = fxreplay.background(timeline, [(40_000, 60_000)], 1000)
        self.assertEqual(false_alarms, 1)  # the one at 80 s; the one at 50 s is inside the trial window
        self.assertEqual(ms, 78_000)      # 99 steps of 1 s, 21 of them starting inside the window (40..60)

    def test_background_caps_gaps(self):
        ms, _ = fxreplay.background([(0, "NORMAL"), (1000, "NORMAL"), (600_000, "NORMAL")], [], 1000)
        self.assertEqual(ms, 1000 + 3000)

    def test_fidelity_matches_within_tolerance(self):
        events = [{"t_ms": 10_400, "type": "state_change", "to": "ELEVATED"},
                  {"t_ms": 25_000, "type": "state_change", "to": "CRITICAL"},
                  {"t_ms": 12_000, "type": "config_change", "key": "fx_enter"}]
        transitions = fxreplay.transitions_of(self.timeline)
        f = fxreplay.fidelity(events, transitions, 500, (0, 200_000))
        self.assertEqual((f["node_transitions"], f["matched"], f["config_changes_in_capture"]), (2, 1, 1))
        self.assertEqual(f["unmatched"], [{"t_s": 25.0, "to": "CRITICAL"}])

    def test_summaries_carry_intervals_and_every_matrix_row(self):
        trials = [
            {"class": "hazard", "category": "gas", "outcome": "detected", "tta_s": 20.0},
            {"class": "hazard", "category": "gas", "outcome": "missed", "tta_s": None},
            {"class": "nuisance", "category": "aerosol", "outcome": "false_alarm", "tta_s": None},
            {"class": "nuisance", "category": "cooking", "outcome": "elevated", "tta_s": None},
        ]
        hazard, nuisance = fxreplay.summarize_trials(trials)
        self.assertEqual((hazard["detection"]["k"], hazard["detection"]["n"]), (1, 2))
        self.assertEqual(hazard["tta_s"]["median"], 20.0)
        self.assertEqual(nuisance["false_alarm"]["k"], 1)
        self.assertEqual(list(nuisance["by_category"])[:6], fxreplay.NUISANCE_CATEGORIES)
        self.assertEqual(nuisance["by_category"]["dust"]["false_alarm"]["n"], 0)


class ConfigTest(unittest.TestCase):
    def test_sweeps_multiply(self):
        configs = fxreplay.build_configs({"fx_crit_ms": 12000}, [fxreplay.parse_sweep("fx_mos_pair=1,0"),
                                                                  fxreplay.parse_sweep("fx_flame_alone=1,0")])
        self.assertEqual(len(configs), 4)
        self.assertEqual(configs[0][0], "fx_crit_ms=12000")
        self.assertEqual(configs[3][0], "fx_crit_ms=12000, fx_flame_alone=0, fx_mos_pair=0")
        self.assertEqual(fxreplay.build_configs({}, [])[0][0], "node defaults")

    def test_unknown_keys_and_bad_values_are_refused(self):
        with self.assertRaises(ValueError):
            fxreplay.parse_assignment("fx_nope=1")
        with self.assertRaises(ValueError):
            fxreplay.parse_assignment("fx_enter=0.6")

    def test_inconsistent_settings_are_caught_before_replay(self):
        self.assertIsNone(check_settings({}))
        self.assertIsNotNone(check_settings({"fx_exit": 60}))
        self.assertIsNotNone(check_settings({"fx_crit_ms": 1000}))

    def test_config_line_order_and_units(self):
        line = config_line({"fx_warmup_s": 0, "fx_mos_pair": 0})
        self.assertEqual(line, "config 0.60 0.40 5000 10000 1 0.90 3000 0 0.75 0.30 0.50 0")
        self.assertEqual(len(DEFAULT_SETTINGS), 12)


@unittest.skipUnless(available(), "fusion engine not built (make -C firmware sim, or FIREX_FUSION_CLI)")
class EngineReplayTest(unittest.TestCase):
    def test_replay_reproduces_a_synthetic_capture(self):
        try:
            sys.path.insert(0, str(HERE.parent / "simulator"))
            import simulate
        except (ImportError, SystemExit):
            self.skipTest("PyYAML not installed")
        with tempfile.TemporaryDirectory() as tmp:
            paths = simulate.write_captures([simulate.SCENARIOS / "gas-leak-closed-room.yaml",
                                             simulate.SCENARIOS / "burnt-toast.yaml"], Path(tmp), repeat=1)
            captures = [fxreplay.load_capture(p) for p in paths]
            doc = fxreplay.replay(captures, fxreplay.build_configs({}, []))
        cfg = doc["configs"][0]
        self.assertTrue(doc["synthetic"])
        self.assertEqual(cfg["fidelity"]["matched"], cfg["fidelity"]["node_transitions"])
        self.assertEqual((cfg["hazard"]["detection"]["k"], cfg["hazard"]["detection"]["n"]), (1, 1))
        self.assertEqual((cfg["nuisance"]["false_alarm"]["k"], cfg["nuisance"]["elevated"]["k"]), (0, 1))


if __name__ == "__main__":
    unittest.main()
