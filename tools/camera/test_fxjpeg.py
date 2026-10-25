import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fxjpeg import FrameError, encode_frame, read_frame  # noqa: E402

JPEG = b"\xff\xd8\xff\xe0" + bytes(range(256)) * 4 + b"\n\nFXEND\n" + b"\xff\xd9"


class FrameTest(unittest.TestCase):
    def test_frame_after_boot_noise(self):
        wire = b"ets Jun  8 2016 00:22:57\r\nFXREADY firex-cam 0.2.0-m1 bench camera=ok\n" + encode_frame(JPEG, 640, 480)
        jpeg, meta = read_frame(io.BytesIO(wire))
        self.assertEqual(jpeg, JPEG)  # a payload containing newlines and FXEND survives
        self.assertEqual((meta["width"], meta["height"], meta["length"]), (640, 480, len(JPEG)))

    def test_hash_mismatch_is_rejected(self):
        wire = bytearray(encode_frame(JPEG, 640, 480))
        header_end = wire.index(b"\n") + 1
        wire[header_end + 10] ^= 0xFF  # one corrupted byte in transit
        with self.assertRaisesRegex(FrameError, "sha256 mismatch"):
            read_frame(io.BytesIO(bytes(wire)))

    def test_short_frame(self):
        wire = encode_frame(JPEG, 640, 480)
        with self.assertRaisesRegex(FrameError, "short frame"):
            read_frame(io.BytesIO(wire[: len(wire) // 2]))

    def test_camera_error(self):
        with self.assertRaisesRegex(FrameError, "capture_failed"):
            read_frame(io.BytesIO(b"FXERR capture_failed\n"))

    def test_not_a_jpeg(self):
        with self.assertRaisesRegex(FrameError, "not a JPEG"):
            read_frame(io.BytesIO(encode_frame(b"plain bytes, no markers", 1, 1)))

    def test_silence(self):
        with self.assertRaisesRegex(FrameError, "no frame header"):
            read_frame(io.BytesIO(b""), max_skip_lines=5)


if __name__ == "__main__":
    unittest.main()
