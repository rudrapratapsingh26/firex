import argparse
import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sign  # noqa: E402


def signed_image(body: bytes, key: bytes = b"\x11" * 776, blocks: int = 1) -> bytes:
    """A synthetic image with v2 signature blocks, laid out as espsecure writes them."""
    padded = body + b"\xff" * (-len(body) % sign.SECTOR)
    sector = b""
    for _ in range(blocks):
        block = bytearray(sign.BLOCK)
        block[0], block[1] = sign.MAGIC, sign.VERSION
        block[4:36] = hashlib.sha256(padded).digest()
        block[sign.KEY_START:sign.KEY_END] = key
        block[sign.CRC_AT:sign.CRC_AT + 4] = zlib.crc32(bytes(block[:sign.CRC_AT])).to_bytes(4, "little")
        sector += bytes(block)
    sector += b"\xff" * (sign.SECTOR - len(sector))
    return padded + sector


# flasher_args.json of a secure boot build: no bootloader entry (confirmed on IDF v5.5.5 builds).
SECURE_BOOT_FLASHER_ARGS = {
    "write_flash_args": ["--flash_mode", "dio", "--flash_size", "keep", "--flash_freq", "80m"],
    "flash_files": {"0x20000": "firex_node.bin", "0x10000": "partition_table/partition-table.bin",
                    "0x18000": "ota_data_initial.bin"},
    "app": {"offset": "0x20000", "file": "firex_node.bin", "encrypted": "false"},
    "partition-table": {"offset": "0x10000", "file": "partition_table/partition-table.bin", "encrypted": "false"},
    "otadata": {"offset": "0x18000", "file": "ota_data_initial.bin", "encrypted": "false"},
    "extra_esptool_args": {"after": "no_reset", "before": "default_reset", "stub": False, "chip": "esp32s3"},
}


class SignatureBlockTest(unittest.TestCase):
    def test_a_signed_image_has_one_good_block_and_its_key_digest(self):
        data = signed_image(b"\xe9" + b"\x00" * 5000)
        blocks = sign.signature_blocks(data)
        self.assertEqual(len(blocks), 1)
        self.assertTrue(blocks[0].crc_ok and blocks[0].digest_ok)
        self.assertEqual(blocks[0].key_digest, hashlib.sha256(b"\x11" * 776).hexdigest())

    def test_an_unsigned_image_has_none(self):
        self.assertEqual(sign.signature_blocks(b"\xe9" + b"\x00" * 8191), [])
        self.assertEqual(sign.signature_blocks(b"\xe9" * 100), [])

    def test_an_edited_image_no_longer_matches_its_block(self):
        data = bytearray(signed_image(b"\xe9" + b"\x00" * 5000))
        data[100] ^= 0x01
        block = sign.signature_blocks(bytes(data))[0]
        self.assertTrue(block.crc_ok)
        self.assertFalse(block.digest_ok)

    def test_a_damaged_block_fails_its_crc(self):
        data = bytearray(signed_image(b"\xe9" + b"\x00" * 5000))
        data[-sign.SECTOR + 40] ^= 0x01  # inside the public key
        self.assertFalse(sign.signature_blocks(bytes(data))[0].crc_ok)


class FlasherArgsTest(unittest.TestCase):
    def test_a_secure_boot_build_puts_the_bootloader_at_the_chips_fixed_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "flasher_args.json").write_text(json.dumps(SECURE_BOOT_FLASHER_ARGS))
            fa = sign.load_flasher_args(tmp)
        self.assertEqual((fa.chip, fa.app_offset, fa.bootloader_offset), ("esp32s3", 0x20000, 0x0))
        self.assertIsNone(fa.bootloader_file)
        self.assertEqual(fa.partition_table_offset, 0x10000)
        self.assertFalse(fa.stub)

    def test_a_plain_build_keeps_its_bootloader_entry(self):
        doc = json.loads(json.dumps(SECURE_BOOT_FLASHER_ARGS))
        doc["extra_esptool_args"]["chip"] = "esp32"
        doc["bootloader"] = {"offset": "0x1000", "file": "bootloader/bootloader.bin", "encrypted": "false"}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "flasher_args.json")
            path.write_text(json.dumps(doc))
            fa = sign.load_flasher_args(path)
        self.assertEqual((fa.bootloader_offset, fa.bootloader_file), (0x1000, "bootloader/bootloader.bin"))


class CommandTest(unittest.TestCase):
    def sign_with_fake_espsecure(self, tmp: Path, name: str, **extra) -> str:
        image = tmp / name
        image.write_bytes(b"\xe9" + b"\x00" * 5000)
        out = tmp / (image.stem + "-signed.bin")

        def fake_run(args):
            Path(args[args.index("--output") + 1]).write_bytes(signed_image(image.read_bytes()))
            return 0

        args = argparse.Namespace(image=str(image), key="release.pem", out=None, bootloader=False, build=None)
        vars(args).update(extra)
        buf = io.StringIO()
        original = sign.run
        sign.run = fake_run
        try:
            with contextlib.redirect_stdout(buf):
                self.assertEqual(sign.cmd_sign(args), 0)
        finally:
            sign.run = original
        self.assertTrue(out.exists())
        return buf.getvalue()

    def test_signing_an_app_without_a_build_keeps_the_dev_kit_hint(self):
        with tempfile.TemporaryDirectory() as tmp:
            text = self.sign_with_fake_espsecure(Path(tmp), "firex_node.bin")
        self.assertIn("write_flash 0x10000", text)
        self.assertIn("firmware add", text)

    def test_signing_with_a_build_takes_its_offsets(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "flasher_args.json").write_text(json.dumps(SECURE_BOOT_FLASHER_ARGS))
            app = self.sign_with_fake_espsecure(Path(tmp), "firex_node.bin", build=tmp)
            boot = self.sign_with_fake_espsecure(Path(tmp), "bootloader.bin", build=tmp)
        self.assertIn("--chip esp32s3 write_flash 0x20000", app)
        self.assertIn("write_flash 0x0 ", boot)
        self.assertNotIn("firmware add", boot)

    def test_check_prints_the_image_kind(self):
        body = bytearray(b"\xe9" + b"\x00" * 5000)
        body[sign.KIND_AT:sign.KIND_AT + 8] = sign.KIND_MAGIC
        body[sign.KIND_AT + 8:sign.KIND_AT + 8 + 14] = b"evt-production"
        self.assertEqual(sign.image_kind(bytes(body)), "evt-production")
        with tempfile.TemporaryDirectory() as tmp:
            marked = Path(tmp, "node.bin")
            marked.write_bytes(signed_image(bytes(body)))
            plain = Path(tmp, "bootloader.bin")
            plain.write_bytes(signed_image(b"\xe9" + b"\x00" * 5000))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(sign.cmd_check(argparse.Namespace(image=str(marked))), 0)
                self.assertEqual(sign.cmd_check(argparse.Namespace(image=str(plain))), 0)
        self.assertIn("kind evt-production", out.getvalue())
        self.assertIn("kind none", out.getvalue())
        unterminated = bytearray(body)
        unterminated[sign.KIND_AT + 8:sign.KIND_AT + 8 + sign.KIND_LEN] = b"x" * sign.KIND_LEN
        self.assertIsNone(sign.image_kind(bytes(unterminated)))

    def test_digest_prints_only_the_key_digest_of_one_good_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp, "good.bin")
            good.write_bytes(signed_image(b"\xe9" + b"\x00" * 5000, key=b"\x33" * 776))
            two = Path(tmp, "two.bin")
            two.write_bytes(signed_image(b"\xe9" + b"\x00" * 5000, blocks=2))
            unsigned = Path(tmp, "unsigned.bin")
            unsigned.write_bytes(b"\xe9" + b"\x00" * 8191)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(sign.cmd_digest(argparse.Namespace(image=str(good))), 0)
                self.assertEqual(sign.cmd_digest(argparse.Namespace(image=str(two))), 1)
                self.assertEqual(sign.cmd_digest(argparse.Namespace(image=str(unsigned))), 1)
        self.assertEqual(out.getvalue().strip(), hashlib.sha256(b"\x33" * 776).hexdigest())
        self.assertIn("expected exactly one", err.getvalue())


if __name__ == "__main__":
    unittest.main()
