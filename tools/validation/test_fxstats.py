import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxstats  # noqa: E402


class WilsonTest(unittest.TestCase):
    def test_zero_events_in_ten_trials_bounds_the_rate_below_28_percent(self):
        lo, hi = fxstats.wilson(0, 10)
        self.assertEqual(lo, 0.0)
        self.assertAlmostEqual(hi, 0.2775, places=3)

    def test_symmetric_about_one_half(self):
        lo, hi = fxstats.wilson(5, 10)
        self.assertAlmostEqual(lo + hi, 1.0, places=9)
        self.assertAlmostEqual(lo, 0.2366, places=3)

    def test_no_trials_says_nothing(self):
        self.assertEqual(fxstats.wilson(0, 0), (0.0, 1.0))

    def test_impossible_counts_are_refused(self):
        with self.assertRaises(ValueError):
            fxstats.wilson(3, 2)


class PercentileTest(unittest.TestCase):
    def test_linear_interpolation(self):
        values = [1.0, 2.0, 3.0, 4.0]
        self.assertEqual(fxstats.percentile(values, 50), 2.5)
        self.assertAlmostEqual(fxstats.percentile(values, 90), 3.7)
        self.assertEqual(fxstats.percentile(values, 0), 1.0)
        self.assertEqual(fxstats.percentile(values, 100), 4.0)

    def test_empty(self):
        self.assertIsNone(fxstats.percentile([], 50))
        self.assertEqual(fxstats.summarize([])["n"], 0)

    def test_summary(self):
        s = fxstats.summarize([10.0, 12.0, 11.0])
        self.assertEqual((s["n"], s["min"], s["median"], s["max"]), (3, 10.0, 11.0, 12.0))
        self.assertAlmostEqual(s["mean"], 11.0)


class PoissonTest(unittest.TestCase):
    def test_zero_events_in_one_unit_bounds_the_rate_below_3_69(self):
        lo, hi = fxstats.poisson_ci(0, 1.0)
        self.assertEqual(lo, 0.0)
        self.assertAlmostEqual(hi, 3.689, places=3)

    def test_five_events_matches_the_garwood_table(self):
        lo, hi = fxstats.poisson_ci(5, 1.0)
        self.assertAlmostEqual(lo, 1.6235, delta=0.002)
        self.assertAlmostEqual(hi, 11.668, delta=0.002)

    def test_exposure_scales_the_interval(self):
        lo, hi = fxstats.poisson_ci(5, 10.0)
        self.assertAlmostEqual(hi, 1.1668, delta=0.0002)
        r = fxstats.rate_per(3, 51.0, per=100.0)
        self.assertAlmostEqual(r["rate"], 300.0 / 51.0)
        self.assertLess(r["ci95"][0], r["rate"])
        self.assertGreater(r["ci95"][1], r["rate"])

    def test_no_exposure_has_no_rate(self):
        self.assertIsNone(fxstats.rate_per(0, 0.0)["rate"])
        with self.assertRaises(ValueError):
            fxstats.poisson_ci(1, 0.0)


class FormatTest(unittest.TestCase):
    def test_rate_text(self):
        self.assertEqual(fxstats.rate_text(fxstats.rate(0, 10)), "0/10 (0 %, 95 % CI 0-28 %)")
        self.assertEqual(fxstats.rate_text(fxstats.rate(0, 0)), "no trials")

    def test_table_escapes_pipes(self):
        table = fxstats.md_table(["a", "b"], [["x|y", 1]])
        self.assertIn("x\\|y", table)
        self.assertEqual(len(table.splitlines()), 3)


if __name__ == "__main__":
    unittest.main()
