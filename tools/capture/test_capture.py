import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

# Loaded by path: under `unittest discover -s tools` the name `capture` is this package.
_spec = importlib.util.spec_from_file_location("fx_capture_tool", Path(__file__).resolve().parent / "capture.py")
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)
CaptureWriter = _module.CaptureWriter


class FakeClock:
    def __init__(self, wall: float):
        self.wall = wall
        self.mono = 100.0

    def tick(self, seconds: float) -> None:
        self.wall += seconds
        self.mono += seconds


class CaptureWriterTest(unittest.TestCase):
    def test_host_times_on_every_row_and_event(self):
        clock = FakeClock(1_790_000_000.0)
        with tempfile.TemporaryDirectory() as tmp:
            w = CaptureWriter(Path(tmp) / "run", wall=lambda: clock.wall, monotonic=lambda: clock.mono)
            w.feed("S,500,0.1,1")  # before any header: not written
            w.feed("H,t_ms,gas,gas_ok")
            clock.tick(1.0)
            w.feed("S,1000,0.100,1")
            clock.tick(0.25)
            w.feed('E,1200,{"type":"mark","text":"gas in"}')
            w.feed("H,t_ms,gas,gas_ok")  # the stream restarted after a reboot
            clock.tick(1.0)
            w.feed("firex> S,300,0.050,1")
            w.close()

            with open(w.csv_path, newline="", encoding="utf-8") as handle:
                rows = list(csv.reader(handle))
            self.assertEqual(rows[0], ["t_ms", "gas", "gas_ok", "host_s", "host_unix_ms"])
            self.assertEqual(rows[1], ["1000", "0.100", "1", "1.0", "1790000001000"])
            self.assertEqual(rows[2][0], "300")
            self.assertEqual(len(rows), 3)

            event = json.loads(w.events_path.read_text(encoding="utf-8").splitlines()[0])
            self.assertEqual(event["t_ms"], 1200)
            self.assertEqual(event["host_unix_ms"], 1790000001250)
            self.assertEqual(event["text"], "gas in")
            self.assertEqual((w.samples, w.events), (2, 1))

    def test_append_does_not_repeat_the_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "run"
            w = CaptureWriter(out)
            w.feed("H,t_ms,gas")
            w.feed("S,1,0.1")
            w.close()
            w = CaptureWriter(out, append=True)
            w.feed("H,t_ms,gas")
            w.feed("S,2,0.2")
            w.close()
            lines = w.csv_path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 3)
            self.assertTrue(lines[0].startswith("t_ms,"))


if __name__ == "__main__":
    unittest.main()
