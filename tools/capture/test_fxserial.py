import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxserial import Event, Header, Sample, StreamParser, number  # noqa: E402

HEADER = "H,t_ms,mq2_v,mq2_rs,mq2_ratio,gas,gas_ok"


class StreamParserTest(unittest.TestCase):
    def test_sample_needs_a_header(self):
        p = StreamParser()
        self.assertIsNone(p.feed("S,1000,0.5,9000,1.0,0.000,1"))
        self.assertEqual(p.dropped, 1)
        self.assertIsInstance(p.feed(HEADER), Header)
        s = p.feed("S,2000,0.5,9000,1.0,0.000,1")
        self.assertIsInstance(s, Sample)
        self.assertEqual(s.t_ms, 2000)
        self.assertEqual(s.values["mq2_rs"], "9000")

    def test_prompt_glued_to_a_record(self):
        p = StreamParser()
        p.feed(HEADER)
        s = p.feed("firex> S,3000,0.6,8000,0.9,0.046,1\r\n")
        self.assertIsInstance(s, Sample)
        self.assertEqual(s.values["gas_ok"], "1")

    def test_column_count_mismatch_is_dropped(self):
        p = StreamParser()
        p.feed(HEADER)
        self.assertIsNone(p.feed("S,3000,0.6,8000"))
        self.assertEqual(p.dropped, 1)

    def test_events_and_noise(self):
        p = StreamParser()
        self.assertIsNone(p.feed("I (312) firex: something"))
        self.assertIsNone(p.feed("  mq2    [mq2_v,mq2_rs,mq2_ratio] 0.5,9000,1.0"))
        e = p.feed('E,4200,{"type":"mark","text":"gas in"}')
        self.assertIsInstance(e, Event)
        self.assertEqual(e.data["text"], "gas in")
        self.assertEqual(len(p.events), 1)
        self.assertIsNone(p.feed("E,4300,{not json"))
        self.assertEqual(p.dropped, 1)

    def test_number(self):
        self.assertEqual(number("1.5"), 1.5)
        self.assertIsNone(number(""))
        self.assertIsNone(number("checksum"))


if __name__ == "__main__":
    unittest.main()
