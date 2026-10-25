import hashlib
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxexport import ExportError, ExportParser, export_lines  # noqa: E402

VECTOR = Path(__file__).resolve().parents[2] / "ledger" / "vectors" / "01-valid-chain.json"


class LogExportTest(unittest.TestCase):
    def test_round_trip_of_the_reference_chain(self):
        vector = json.loads(VECTOR.read_text(encoding="utf-8"))
        parser = ExportParser()
        for i, line in enumerate(export_lines(vector)):
            parser.feed(("firex> " if i == 0 else "") + line + "\r\n")
        bundle = parser.bundle()
        self.assertEqual(bundle["public_key_pem"], vector["public_key_pem"])
        self.assertEqual(bundle["log"], vector["log"])
        self.assertEqual(bundle["media"], [])

    def test_ignores_console_output_and_logs(self):
        parser = ExportParser()
        self.assertIsNone(parser.feed("I (1234) ledger: something"))
        self.assertIsNone(parser.feed("E,100,{\"type\":\"boot\"}"))

    def test_short_export_is_refused(self):
        parser = ExportParser()
        parser.feed("X,begin,abcd")
        parser.feed("X,key,-----BEGIN PUBLIC KEY-----\\nAAAA\\n-----END PUBLIC KEY-----\\n")
        parser.feed('X,entry,{"seq":0}')
        with self.assertRaisesRegex(ExportError, "reported 2 entries, 1 arrived"):
            parser.feed("X,end,2")
        with self.assertRaisesRegex(ExportError, "did not finish"):
            parser.bundle()


class MediaExportTest(unittest.TestCase):
    def test_media_transfer_is_checked(self):
        data = bytes(range(256)) * 3
        sha = hashlib.sha256(data).hexdigest()
        parser = ExportParser()
        parser.feed(f"M,file,b1-c1-f0-abcd.jpg,{len(data)},{sha}")
        parser.feed("M,count,1")
        parser.feed(f"M,begin,b1-c1-f0-abcd.jpg,{len(data)}")
        for i in range(0, len(data), 256):
            parser.feed("M,data," + data[i:i + 256].hex())
        parser.feed(f"M,end,{sha}")
        self.assertEqual(parser.files["b1-c1-f0-abcd.jpg"].data, data)
        self.assertEqual(parser.listed["b1-c1-f0-abcd.jpg"].sha256, sha)

    def test_damaged_transfer_raises(self):
        data = b"\xff\xd8 not quite a picture \xff\xd9"
        parser = ExportParser()
        parser.feed(f"M,begin,x.jpg,{len(data)}")
        parser.feed("M,data," + data[:-1].hex())
        with self.assertRaisesRegex(ExportError, "transfer damaged"):
            parser.feed("M,end," + hashlib.sha256(data).hexdigest())

    def test_node_error(self):
        with self.assertRaisesRegex(ExportError, "no such file"):
            ExportParser().feed("M,error,no such file")


if __name__ == "__main__":
    unittest.main()
