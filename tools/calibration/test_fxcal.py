import csv
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxcal  # noqa: E402


class LoadingTest(unittest.TestCase):
    def test_capture_and_events_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            csv_path = Path(tmp) / "run.csv"
            with open(csv_path, "w", newline="", encoding="utf-8") as handle:
                writer = csv.writer(handle)
                writer.writerow(["t_ms", "mq2_rs", "gas_ok", "dht_status", "host_s"])
                writer.writerow([1000, 9000.5, 1, "ok", 0.1])
                writer.writerow([2000, "", 0, "checksum", 1.1])
            events_path = Path(tmp) / "run.events.jsonl"
            events_path.write_text(json.dumps({"t_ms": 1500, "type": "mark", "text": "gas in"}) + "\n")

            cap = fxcal.load_capture(csv_path)
            self.assertEqual(cap["t_s"], [1.0, 2.0])
            self.assertEqual(cap["mq2_rs"], [9000.5, None])
            self.assertEqual(cap["dht_status"], ["ok", "checksum"])
            self.assertEqual(fxcal.series(cap, "mq2_rs"), ([1.0], [9000.5]))
            self.assertEqual(fxcal.series(cap, "mq2_rs", valid="gas_ok"), ([1.0], [9000.5]))

            events = fxcal.load_events(events_path)
            self.assertEqual(events[0]["t_s"], 1.5)


class WarmupAndBaselineTest(unittest.TestCase):
    def test_warmup_time_of_an_exponential_settle(self):
        ts = [float(t) for t in range(0, 900)]
        vs = [10000.0 * (1 + 2 * math.exp(-t / 60)) for t in ts]
        settle = fxcal.warmup_time(ts, vs, tolerance=0.02, hold_s=60)
        # spread over [T, T+60] is 2 e^(-T/60) (1 - e^-1) of the value: 2 % at T ~ 249 s
        self.assertIsNotNone(settle)
        self.assertTrue(240 <= settle <= 252, settle)

    def test_warmup_never_settles_in_a_short_capture(self):
        ts = [float(t) for t in range(0, 120)]
        vs = [10000.0 * (1 + 2 * math.exp(-t / 60)) for t in ts]
        self.assertIsNone(fxcal.warmup_time(ts, vs, tolerance=0.02, hold_s=60))

    def test_baseline_matches_the_firmware_rule(self):
        values = [10000.0] * 29 + [50000.0]
        middle, spread = fxcal.baseline(values)
        self.assertEqual(middle, 10000.0)
        self.assertEqual(spread, 0.0)  # one spike sits outside p10..p90


class ResponseTest(unittest.TestCase):
    def test_rise_times_of_a_first_order_step(self):
        ts, vs = fxcal.synthetic_step(t0=100, tau=10, base=0.05, extreme=0.85, duration=300)
        r = fxcal.response_times(ts, vs, t_start=100)
        self.assertEqual(r.direction, "rise")
        self.assertAlmostEqual(r.t10, -10 * math.log(0.9), delta=0.3)
        self.assertAlmostEqual(r.t50, 10 * math.log(2), delta=0.3)
        self.assertAlmostEqual(r.t90, 10 * math.log(10), delta=0.3)

    def test_fall_of_the_rs_ratio(self):
        ts, vs = fxcal.synthetic_step(t0=60, tau=25, base=1.0, extreme=0.3, duration=400)
        r = fxcal.response_times(ts, vs, t_start=60)
        self.assertEqual(r.direction, "fall")
        self.assertAlmostEqual(r.t90, 25 * math.log(10), delta=0.3)

    def test_no_change_has_no_response_time(self):
        ts = [float(t) for t in range(100)]
        r = fxcal.response_times(ts, [1.0] * 100, t_start=50)
        self.assertIsNone(r.t90)

    def test_window_without_samples_before_start(self):
        with self.assertRaises(ValueError):
            fxcal.response_times([10.0, 11.0], [1.0, 0.5], t_start=10.0)


class FanComparisonTest(unittest.TestCase):
    def test_marks_fan_state_and_comparison(self):
        cap, events = fxcal.demo_fan_runs()
        pairs = fxcal.mark_pairs(events)
        self.assertEqual(len(pairs), 4)
        on, off = [], []
        ts, vs = fxcal.series(cap, "mq2_ratio")
        for t_in, t_out in pairs:
            response = fxcal.response_times(ts, vs, t_in, t_out)
            (on if fxcal.fan_state_at(events, t_in) == "on" else off).append(response)
        summary = fxcal.compare_fan(on, off)
        self.assertEqual((summary["n_on"], summary["n_off"]), (2, 2))
        self.assertGreater(summary["off_median_s"], summary["on_median_s"])
        self.assertAlmostEqual(summary["on_median_s"], 8.25 * math.log(10), delta=1.5)

    def test_unclosed_mark(self):
        events = [{"type": "mark", "text": "gas in", "t_s": 5.0}]
        self.assertEqual(fxcal.mark_pairs(events), [(5.0, None)])


class FitTest(unittest.TestCase):
    def test_linear_fit(self):
        slope, intercept, r2 = fxcal.linear_fit([40, 60, 80], [1.0, 0.9, 0.8])
        self.assertAlmostEqual(slope, -0.005)
        self.assertAlmostEqual(intercept, 1.2)
        self.assertAlmostEqual(r2, 1.0)


if __name__ == "__main__":
    unittest.main()
