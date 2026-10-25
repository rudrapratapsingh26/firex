import random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxframes  # noqa: E402


def room(width: int = 64, height: int = 48) -> list[list[float]]:
    """A lit test scene: a bright window, a dark doorway and a fine checkered rug."""
    image = []
    for y in range(height):
        row = []
        for x in range(width):
            v = 110.0
            if 8 <= x < 24 and 6 <= y < 20:
                v = 230.0
            elif 44 <= x < 56 and 10 <= y < 44:
                v = 30.0
            elif y >= 34 and x < 40:
                v = 170.0 if (x // 2 + y // 2) % 2 else 60.0
            row.append(v)
        image.append(row)
    return image


def blur(image, passes: int = 2):
    for _ in range(passes):
        h, w = len(image), len(image[0])
        image = [[sum(image[min(h - 1, max(0, y + dy))][min(w - 1, max(0, x + dx))]
                      for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9.0 for x in range(w)] for y in range(h)]
    return image


class MeasureTest(unittest.TestCase):
    def test_flat_image(self):
        flat = [[100.0] * 10 for _ in range(8)]
        self.assertEqual(fxframes.mean_luma(flat), 100.0)
        self.assertEqual(fxframes.contrast(flat), 0.0)
        self.assertEqual(fxframes.sharpness(flat), 0.0)
        self.assertEqual(fxframes.similarity(flat, flat), 0.0)

    def test_resize_averages_blocks_and_grows(self):
        image = [[0, 0, 100, 100], [0, 0, 100, 100]]
        self.assertEqual(fxframes.resize(image, 2, 1), [[0.0, 100.0]])
        self.assertEqual(fxframes.resize([[7]], 2, 2), [[7.0, 7.0], [7.0, 7.0]])

    def test_similarity_ignores_brightness_and_gain(self):
        a = room()
        b = [[0.5 * v + 20 for v in row] for row in a]
        self.assertAlmostEqual(fxframes.similarity(a, b), 1.0, places=9)
        inverted = [[255 - v for v in row] for row in a]
        self.assertAlmostEqual(fxframes.similarity(a, inverted), -1.0, places=9)


class FlagTest(unittest.TestCase):
    def setUp(self):
        self.ref = fxframes.reference([room(), room()])

    def test_the_clear_room_is_clear(self):
        m = fxframes.assess(room(), self.ref)
        self.assertEqual(m["flags"], [])
        self.assertAlmostEqual(m["similarity"], 1.0, places=6)

    def test_an_unlit_room_is_dark_but_still_the_room(self):
        dim = [[0.12 * v for v in row] for row in room()]
        m = fxframes.assess(dim, self.ref)
        self.assertEqual(m["flags"], ["dark"])
        self.assertGreater(m["similarity"], 0.99)

    def test_smoke_obscures_the_view(self):
        rng = random.Random(4)
        smoke = [[0.05 * v + 150 + rng.uniform(-14, 14) for v in row] for row in room()]
        m = fxframes.assess(smoke, self.ref)
        self.assertLess(m["similarity"], 0.5)
        self.assertIn("obscured", m["flags"])
        self.assertNotIn("dark", m["flags"])

    def test_thin_haze_is_not_obscured(self):
        haze = [[0.6 * v + 80 for v in row] for row in room()]
        self.assertNotIn("obscured", fxframes.assess(haze, self.ref)["flags"])

    def test_a_defocused_frame_is_blurred(self):
        m = fxframes.assess(blur(room()), self.ref)
        self.assertIn("blurred", m["flags"])

    def test_frames_of_another_size_are_compared_after_resizing(self):
        big = fxframes.resize(room(), 128, 96)
        self.assertGreater(fxframes.assess(big, self.ref)["similarity"], 0.95)


class FrameCheckTest(unittest.TestCase):
    def test_decoding_real_files(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        import frame_check

        def save(path: Path, image) -> None:
            img = Image.new("L", (len(image[0]), len(image)))
            img.putdata([int(max(0, min(255, v))) for row in image for v in row])
            img.save(path)

        with tempfile.TemporaryDirectory() as tmp:
            ref_dir, trial = Path(tmp) / "ref", Path(tmp) / "unlit"
            ref_dir.mkdir()
            trial.mkdir()
            save(ref_dir / "b1-c1-f0-aaaa.png", room())
            save(trial / "b1-c2-f0-bbbb.png", [[0.1 * v for v in row] for row in room()])
            save(trial / "b1-c2-f1-cccc.png", room())
            report = frame_check.analyse([ref_dir], [trial], synthetic=True)
            self.assertEqual([f["index"] for f in report["frames"]], [0, 1])
            self.assertEqual(report["frames"][0]["flags"], ["dark"])
            self.assertEqual(report["frames"][1]["flags"], [])
            self.assertEqual(report["frames"][0]["trial"], "unlit")
            self.assertIn("SYNTHETIC", frame_check.markdown(report))


if __name__ == "__main__":
    unittest.main()
