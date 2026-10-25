"""Host tests for the factory provisioning tool. No board, no serial port, no esptool, no sleeps:
a fake unit answers the console, a fake runner stands in for esptool and espefuse, and a fake
clock advances only when the tool waits."""
import base64
import contextlib
import hashlib
import io
import json
import shutil
import sys
import tempfile
import unittest
import zlib
from collections import deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fxfactory as ff  # noqa: E402
import provision  # noqa: E402

sign = ff.sign
TESTDATA = HERE / "testdata"
REPO = HERE.parent.parent

KEY = b"\x22" * 776
DIGEST = hashlib.sha256(KEY).hexdigest()
OTHER_KEY = b"\x44" * 776
VERSION = "0.6.0-evt"
CAM_VERSION = "0.6.0-cam"
PUB = b"\x04" + bytes(range(1, 65))
PAIRING = "7K3QD9M2XA"


def signed_image(body: bytes, key: bytes = KEY, blocks: int = 1) -> bytes:
    padded = body + b"\xff" * (-len(body) % sign.SECTOR)
    sector = b""
    for _ in range(blocks):
        block = bytearray(sign.BLOCK)
        block[0], block[1] = sign.MAGIC, sign.VERSION
        block[4:36] = hashlib.sha256(padded).digest()
        block[sign.KEY_START:sign.KEY_END] = key
        block[sign.CRC_AT:sign.CRC_AT + 4] = zlib.crc32(bytes(block[:sign.CRC_AT])).to_bytes(4, "little")
        sector += bytes(block)
    return padded + sector + b"\xff" * (sign.SECTOR - len(sector))


def app_body(project: str = "firex_node", version: str = VERSION, kind: str | None = "evt-production") -> bytes:
    body = bytearray(0x2000)
    body[0] = 0xE9
    body[0x20:0x24] = ff.APP_DESC_MAGIC.to_bytes(4, "little")
    body[0x30:0x30 + len(version)] = version.encode()
    body[0x50:0x50 + len(project)] = project.encode()
    if kind is not None:  # firmware/main/image_kind.c, in the custom app description at 0x120
        body[sign.KIND_AT:sign.KIND_AT + 8] = sign.KIND_MAGIC
        body[sign.KIND_AT + 8:sign.KIND_AT + 8 + len(kind)] = kind.encode()
    return bytes(body)


def boot_body() -> bytes:
    return b"\xe9" + b"\x03" * 0x17FF


def pem(pub: bytes) -> list[str]:
    b64 = base64.b64encode(ff.SPKI_P256_PREFIX + pub).decode()
    return ["-----BEGIN PUBLIC KEY-----"] + [b64[i:i + 64] for i in range(0, len(b64), 64)] + ["-----END PUBLIC KEY-----"]


def blank_config(serial: bytes = bytes(range(0x01, 0x0A))) -> bytearray:
    cfg = bytearray(128)
    cfg[0:4], cfg[8:13] = serial[:4], serial[4:9]
    cfg[20:22] = (0x8F8F).to_bytes(2, "little")  # a factory default that is not FireX's
    cfg[22:24] = (0x8F8F).to_bytes(2, "little")
    cfg[96:98] = (0x0013).to_bytes(2, "little")
    cfg[98:100] = (0x0013).to_bytes(2, "little")
    cfg[86] = cfg[87] = 0x55
    cfg[88:90] = b"\xff\xff"
    return cfg


def apply(cfg: bytearray, step: str) -> None:
    if step == "write_config":
        for slot in (0, 1):
            cfg[20 + 2 * slot:22 + 2 * slot] = ff.SLOT_CONFIG_REQUIRED.to_bytes(2, "little")
            cfg[96 + 2 * slot:98 + 2 * slot] = ff.KEY_CONFIG_REQUIRED.to_bytes(2, "little")
    elif step == "lock_config":
        cfg[87] = 0x00
    elif step == "lock_data":
        cfg[86] = 0x00
    elif step == "lock_ledger_slot":
        cfg[88] &= 0xFE


def provisioned_config() -> bytearray:
    cfg = blank_config()
    for step in ("write_config", "lock_config", "lock_data", "lock_ledger_slot"):
        apply(cfg, step)
    return cfg


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self) -> float:
        return self.t

    def advance(self, dt: float) -> None:
        self.t += max(dt, 0.001)


class FakeUnit:
    """A node that answers the console protocol the firmware implements."""

    def __init__(self, clock, *, secured=False, version=VERSION, cfg=None, security=None, digests=None,
                 step_override=None, provision_fail=None, pem_pub=None, camera_answers=True):
        self.clock = clock
        self.q: deque[str] = deque()
        self.secured = secured
        self.version = version
        self.cfg = cfg if cfg is not None else blank_config()
        self.sec = dict(secure_boot="on", flash_encryption="release", jtag="disabled", usb_jtag="disabled",
                        rom_download="secure", nvs_encryption="on", device_key="atecc608b", production="yes")
        self.sec.update(security or {})
        self.digests = digests or [DIGEST, "none", "revoked"]
        self.step_override = step_override
        self.provision_fail = provision_fail
        self.pem_pub = pem_pub
        self.camera_answers = camera_answers
        self.sent: list[str] = []
        self.boots = 0
        self.resets = 0
        self.closed = 0

    def flash(self):
        self.secured = True

    def boot(self):
        self.boots += 1
        self.q.extend(["ESP-ROM:esp32s3-20210327", "I (80) boot: Secure boot V2 is enabled.",
                       f"FireX node {self.version}, boot {self.boots} (power_on)", "pairing code 7K3QD-9M2XA"])

    # Console
    def write_line(self, text):
        self.sent.append(text)
        self.q.append(f"firex> {text}")
        if not self.secured:
            return
        cfg = ff.SeConfig(bytes(self.cfg))
        if text == "security status":
            self.q.extend(f"SEC,{k},{v}" for k, v in self.sec.items())
            self.q.extend(f"SEC,key_digest,{i},{d}" for i, d in enumerate(self.digests))
            self.q.append(f"SEC,firmware,{self.version},{'ab' * 32}")
        elif text == "se status":
            step, why = ff.se_next_step(cfg)
            step = self.step_override or step
            self.q.extend(["SE,backend,atecc608b", "SE,present,yes", f"SE,serial,{cfg.serial}", "SE,revision,00006002",
                           f"SE,config,{cfg.raw.hex()}", f"SE,step,{step}"])
            if step == "refuse":
                self.q.append(f"SE,why,{why}")
            if step == "done":
                self.q.append(f"SE,ledger_pub,{PUB.hex()}")
        elif text == f"se provision {ff.PROVISION_TOKEN}":
            if self.provision_fail:
                self.q.append(f"SE,result,fail,{self.provision_fail}")
                return
            step, why = ff.se_next_step(cfg)
            if step == "refuse":
                self.q.append(f"SE,result,fail,{why}")
                return
            if step == "done":
                self.q.extend([f"SE,ledger_pub,{PUB.hex()}", "SE,result,ok,already"])
                return
            while step not in ("done", "refuse"):
                if step == "genkey_ledger":
                    self.q.append("SE,step,genkey_ledger")
                    step = "lock_ledger_slot"
                apply(self.cfg, step)
                self.q.append(f"SE,step,{step}")
                step, _ = ff.se_next_step(ff.SeConfig(bytes(self.cfg)))
            self.q.extend([f"SE,ledger_pub,{PUB.hex()}", "SE,result,ok"])
            self.boot()
        elif text == "ledger key":
            self.q.extend(pem(self.pem_pub or PUB))
        elif text == "pairing":
            self.q.append("pairing code 7K3QD-9M2XA")
        elif text == "cam ping":
            self.q.append("camera answered" if self.camera_answers else "camera did NOT answer")

    def read_line(self, timeout):
        if self.q:
            return self.q.popleft()
        self.clock.advance(timeout)
        return None

    def reset(self):
        self.resets += 1
        if self.secured:
            self.boot()

    def close(self):
        self.closed += 1


class FakeCamera:
    def __init__(self, clock, *, running=False, version=CAM_VERSION, kind="link-only", status="ok"):
        self.clock, self.running, self.version, self.kind, self.status = clock, running, version, kind, status
        self.q: deque[str] = deque()
        self.closed = 0

    def boot(self):
        self.q.extend(["rst:0x1 (POWERON_RESET)", f"FXREADY firex-cam {self.version} {self.kind} camera={self.status}"])

    def write_line(self, text):
        pass

    def read_line(self, timeout):
        if self.q:
            return self.q.popleft()
        self.clock.advance(timeout)
        return None

    def reset(self):
        if self.running:
            self.boot()

    def close(self):
        self.closed += 1


class FakeRunner:
    def __init__(self, unit, efuse="virgin", camera=None, esptool_rc=0, camera_rc=0):
        self.unit, self.efuse, self.camera = unit, efuse, camera
        self.esptool_rc, self.camera_rc = esptool_rc, camera_rc
        self.calls: list[list[str]] = []

    def __call__(self, args):
        self.calls.append(args)
        if "espefuse" in args:
            if self.efuse == "unreachable":
                return 2, "", "A fatal error occurred: Secure Download Mode is enabled"
            summary = json.loads((TESTDATA / f"espefuse_esp32s3_{'virgin' if self.efuse == 'virgin' else 'secured'}.json")
                                 .read_text())
            if self.efuse == "secured":  # the real fixture's digest is another key; make it ours
                summary["BLOCK_KEY1"]["value"] = " ".join(DIGEST[i:i + 2] for i in range(0, 64, 2))
            Path(args[args.index("--file") + 1]).write_text(json.dumps(summary))
            return 0, "", ""
        if "esptool" in args:
            chip = args[args.index("--chip") + 1]
            if chip == "esp32s3":
                if self.esptool_rc:
                    return self.esptool_rc, "", "A fatal error occurred: Failed to connect to ESP32-S3"
                self.unit.flash()
                self.unit.boot()  # --after hard_reset
                return 0, "Hash of data verified.", ""
            if self.camera_rc:
                return self.camera_rc, "", "A fatal error occurred: Failed to connect to ESP32"
            self.camera.running = True
            self.camera.boot()
            return 0, "", ""
        raise AssertionError(f"unexpected command {args}")

    def flashed(self, chip="esp32s3"):
        return [c for c in self.calls if "esptool" in c and c[c.index("--chip") + 1] == chip]


class Workspace:
    def __init__(self, tmp: Path, *, app_key=KEY, boot_key=KEY, version=VERSION, blocks=1, kind="evt-production"):
        self.tmp = tmp
        self.build = tmp / "build-evt-production"
        self.build.mkdir()
        shutil.copy(TESTDATA / "flasher_args_evt_production.json", self.build / "flasher_args.json")
        self.boot = tmp / "bootloader-signed.bin"
        self.boot.write_bytes(signed_image(boot_body(), boot_key))
        self.app = tmp / "firex_node-signed.bin"
        self.app.write_bytes(signed_image(app_body(version=version, kind=kind), app_key, blocks))
        self.cam_build = tmp / "cam-build-production"
        self.cam_build.mkdir()
        shutil.copy(TESTDATA / "flasher_args_cam_production.json", self.cam_build / "flasher_args.json")
        self.cam_boot = tmp / "cam-bootloader-signed.bin"
        self.cam_boot.write_bytes(signed_image(boot_body()))
        self.cam_app = tmp / "firex_cam-signed.bin"
        self.cam_app.write_bytes(signed_image(app_body("firex_cam", CAM_VERSION, kind=None)))
        self.records = tmp / "records"

    def job(self, serial="EVT-0001", camera=False):
        return ff.UnitJob(port="COM7", build_dir=str(self.build), signed_bootloader=str(self.boot),
                          signed_app=str(self.app), release_key_digest=DIGEST, unit_serial=serial,
                          records_dir=str(self.records), operator="op", station="bench-1", python="python",
                          camera=ff.CameraJob("COM8", str(self.cam_build), str(self.cam_boot), str(self.cam_app))
                          if camera else None)


class StampClock:
    def __init__(self):
        self.n = 0

    def __call__(self):
        self.n += 1
        return f"2026-09-16T10:00:{self.n:02d}Z"


# --- the pure pieces ------------------------------------------------------------------------

class CrcTest(unittest.TestCase):
    def test_cryptoauthlib_vectors(self):
        self.assertEqual(ff.atcrc(bytes([0x07, 0x30, 0x00, 0x00, 0x00])), bytes([0x03, 0x5D]))
        self.assertEqual(ff.atcrc(bytes(range(128))), bytes([0x9F, 0x24]))
        self.assertEqual(ff.SeConfig(bytes(range(128))).crc, "249f")


class SeStepTest(unittest.TestCase):
    def step(self, cfg):
        return ff.se_next_step(ff.SeConfig(bytes(cfg)))

    def test_the_provisioning_sequence(self):
        cfg = blank_config()
        self.assertEqual(self.step(cfg), ("write_config", None))
        apply(cfg, "write_config")
        self.assertEqual(self.step(cfg), ("lock_config", None))
        apply(cfg, "lock_config")
        self.assertEqual(self.step(cfg), ("lock_data", None))
        apply(cfg, "lock_data")
        self.assertEqual(self.step(cfg), ("genkey_ledger", None))
        apply(cfg, "lock_ledger_slot")
        self.assertEqual(self.step(cfg), ("done", None))

    def test_a_chip_someone_else_configured_is_refused(self):
        cfg = blank_config()
        apply(cfg, "lock_config")
        step, why = self.step(cfg)
        self.assertEqual(step, "refuse")
        self.assertIn("FireX did not write", why)

    def test_a_locked_data_zone_under_an_unlocked_config_is_refused(self):
        cfg = blank_config()
        apply(cfg, "lock_data")
        self.assertEqual(self.step(cfg)[0], "refuse")

    def test_slot_0_locked_too_early_can_never_hold_a_key(self):
        cfg = blank_config()
        apply(cfg, "lock_ledger_slot")
        step, why = self.step(cfg)
        self.assertEqual(step, "refuse")
        self.assertIn("unlocked configuration", why)
        cfg = blank_config()
        for step_name in ("write_config", "lock_config", "lock_ledger_slot"):
            apply(cfg, step_name)
        step, why = self.step(cfg)
        self.assertEqual(step, "refuse")
        self.assertIn("before the data zone", why)

    def test_apply_changes_only_the_two_slots(self):
        raw = bytes(blank_config())
        applied = ff.se_config_apply(raw)
        changed = [i for i in range(128) if raw[i] != applied[i]]
        self.assertTrue(set(changed) <= {20, 21, 22, 23, 96, 97, 98, 99})
        self.assertTrue(ff.SeConfig(applied).firex_slots)

    def test_only_the_tls_slot_differing_is_still_foreign(self):
        cfg = blank_config()
        apply(cfg, "write_config")
        cfg[98:100] = (0x0013).to_bytes(2, "little")
        apply(cfg, "lock_config")
        self.assertEqual(self.step(cfg)[0], "refuse")

    def test_the_serial_number_comes_from_bytes_0_3_and_8_12(self):
        self.assertEqual(ff.SeConfig(bytes(blank_config())).serial, "010203040506070809")


class GoldenConfigTest(unittest.TestCase):
    """Cross-language check against firmware/components/secure (written by its golden generator)."""

    def setUp(self):
        path = TESTDATA / "se_config_golden.json"
        if not path.exists():
            self.skipTest(f"{path.name} not present: `make -C firmware se-golden` writes it")
        self.doc = json.loads(path.read_text())

    def test_every_step_case(self):
        cases = self.doc["cases"]
        self.assertGreater(len(cases), 0)
        for case in cases:
            with self.subTest(case["name"]):
                self.assertEqual(ff.se_next_step(ff.SeConfig.from_hex(case["config"]))[0], case["step"])

    def test_constants_crc_and_apply(self):
        self.assertEqual((self.doc["slot_config"], self.doc["key_config"]),
                         (ff.SLOT_CONFIG_REQUIRED, ff.KEY_CONFIG_REQUIRED))
        for vector in self.doc["crc_vectors"]:
            data = bytes.fromhex(vector["data"])
            self.assertEqual(ff.atcrc(data).hex(), vector["crc_le"])
            self.assertEqual(int.from_bytes(ff.atcrc(data), "little"), vector["crc16"])
        applied = ff.se_config_apply(bytes.fromhex(self.doc["factory_config"]))
        self.assertEqual(applied.hex(), self.doc["applied_config"])
        self.assertEqual(ff.SeConfig(applied).crc, f"{self.doc['applied_config_crc16']:04x}")


class EfuseTest(unittest.TestCase):
    def load(self, name):
        return json.loads((TESTDATA / f"espefuse_esp32s3_{name}.json").read_text())

    def test_a_virgin_chip_from_real_espefuse_output(self):
        st = ff.parse_espefuse(self.load("virgin"))
        self.assertFalse(st.secure_boot or st.flash_encryption or st.jtag_disabled)
        self.assertEqual((st.rom_download, st.digests), ("open", {}))
        self.assertEqual(ff.classify_chip(st, DIGEST), ("virgin", None))

    def test_a_secured_chip_from_real_espefuse_output(self):
        st = ff.parse_espefuse(self.load("secured"))
        real = "83815c32bbe009d91020e00772f459290fdba97728a5211de0dff0a6d8cbca60"
        self.assertTrue(st.secure_boot and st.flash_encryption and st.jtag_disabled and st.usb_jtag_disabled)
        self.assertEqual(st.rom_download, "secure")
        self.assertEqual(st.digests, {0: real})
        self.assertEqual(st.revoked, {0: False, 1: True, 2: True})
        self.assertEqual(ff.classify_chip(st, real), ("ours", None))
        kind, why = ff.classify_chip(st, DIGEST)
        self.assertEqual(kind, "refuse")
        self.assertIn("another key", why)

    def test_flash_encryption_without_secure_boot_is_refused(self):
        summary = self.load("virgin")
        summary["SPI_BOOT_CRYPT_CNT"]["raw_value"] = "0x1"
        self.assertEqual(ff.classify_chip(ff.parse_espefuse(summary), DIGEST)[0], "refuse")

    def test_a_digest_burned_by_an_interrupted_first_boot_with_our_key_is_still_virgin(self):
        summary = self.load("secured")
        summary["SECURE_BOOT_EN"]["value"] = False
        summary["SPI_BOOT_CRYPT_CNT"]["raw_value"] = "0x0"
        summary["BLOCK_KEY1"]["value"] = " ".join(DIGEST[i:i + 2] for i in range(0, 64, 2))
        self.assertEqual(ff.classify_chip(ff.parse_espefuse(summary), DIGEST)[0], "virgin")


class ImageTest(unittest.TestCase):
    def test_a_good_app_and_its_description(self):
        info, problems = ff.inspect_signed_image("app.bin", signed_image(app_body()), "app", DIGEST, "firex_node")
        self.assertEqual(problems, [])
        self.assertEqual((info.project, info.version, info.key_digest), ("firex_node", VERSION, DIGEST))

    def test_refusals(self):
        _, wrong_key = ff.inspect_signed_image("a", signed_image(app_body(), OTHER_KEY), "app", DIGEST)
        self.assertTrue(any("not the release key" in p for p in wrong_key))
        _, unsigned = ff.inspect_signed_image("a", app_body(), "app", DIGEST)
        self.assertTrue(any("0 signature blocks" in p for p in unsigned))
        _, two = ff.inspect_signed_image("a", signed_image(app_body(), blocks=2), "app", DIGEST)
        self.assertTrue(any("2 signature blocks" in p for p in two))
        _, camera = ff.inspect_signed_image("a", signed_image(app_body("firex_cam")), "app", DIGEST, "firex_node")
        self.assertTrue(any("expected firex_node" in p for p in camera))
        _, boot = ff.inspect_signed_image("b", signed_image(boot_body()), "bootloader", DIGEST)
        self.assertEqual(boot, [])

    def test_the_image_kind_marker(self):
        info, problems = ff.inspect_signed_image("a", signed_image(app_body()), "app", DIGEST, "firex_node",
                                                 "evt-production")
        self.assertEqual((info.image_kind, problems), ("evt-production", []))
        _, evt = ff.inspect_signed_image("a", signed_image(app_body(kind="evt")), "app", DIGEST, "firex_node",
                                         "evt-production")
        self.assertTrue(any("image kind evt, expected evt-production" in p for p in evt))
        _, none = ff.inspect_signed_image("a", signed_image(app_body(kind=None)), "app", DIGEST, "firex_node",
                                          "evt-production")
        self.assertTrue(any("image kind none" in p for p in none))
        cam, cam_problems = ff.inspect_signed_image("c", signed_image(app_body("firex_cam", kind=None)), "app", DIGEST,
                                                    "firex_cam")
        self.assertEqual((cam.image_kind, cam_problems), (None, []))


class FlashPlanTest(unittest.TestCase):
    def test_the_evt_production_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            ws = Workspace(Path(tmp))
            plan = ff.flash_plan(ws.build, ws.boot, ws.app, "esp32s3")
            offsets = {o: p for o, p in plan.entries}
            self.assertEqual(sorted(offsets), [0x0, 0x10000, 0x18000, 0x20000])
            self.assertEqual(offsets[0x0], str(ws.boot))
            self.assertEqual(offsets[0x20000], str(ws.app))
            self.assertTrue(offsets[0x10000].endswith("partition-table.bin"))
            cmd = plan.command("python", "COM7")
            self.assertIn("--no-stub", cmd)
            self.assertEqual(cmd[cmd.index("--after") + 1], "hard_reset")
            self.assertNotIn("0x8000", cmd)
            self.assertEqual(cmd[cmd.index("write_flash") + 1:cmd.index("write_flash") + 7],
                             ["--flash_mode", "dio", "--flash_size", "keep", "--flash_freq", "80m"])

    def test_the_camera_bootloader_goes_at_0x1000(self):
        with tempfile.TemporaryDirectory() as tmp:
            ws = Workspace(Path(tmp))
            plan = ff.flash_plan(ws.cam_build, ws.cam_boot, ws.cam_app, "esp32")
            self.assertEqual([o for o, _ in plan.entries], [0x1000, 0x10000, 0x20000])
            with self.assertRaises(ValueError):
                ff.flash_plan(ws.cam_build, ws.cam_boot, ws.cam_app, "esp32s3")


class ParseTest(unittest.TestCase):
    def test_a_banner_behind_a_prompt(self):
        b = ff.parse_banner("firex> firex> FireX node 0.6.0-evt, boot 3 (software)")
        self.assertEqual((b.version, b.boot, b.reset_reason), ("0.6.0-evt", 3, "software"))
        self.assertIsNone(ff.parse_banner("FireX node status"))

    def test_device_id_matches_the_ledger_reference(self):
        vector = json.loads((REPO / "ledger" / "vectors" / "01-valid-chain.json").read_text())
        pub = ff.pem_public_key(vector["public_key_pem"])
        self.assertEqual(ff.device_id(pub), vector["log"][0]["device_id"])

    def test_pairing_codes(self):
        self.assertEqual(ff.parse_pairing(["firex> pairing code 7k3qd-9m2xa"]), PAIRING)
        self.assertEqual(ff.normalise_pairing_code("7K3QD-9M2XO"), "7K3QD9M2X0")
        self.assertIsNone(ff.parse_pairing(["pairing code 7K3QD-9M2"]))

    def test_security_problems(self):
        good = ["SEC,secure_boot,on", "SEC,flash_encryption,release", "SEC,jtag,disabled", "SEC,usb_jtag,disabled",
                "SEC,rom_download,disabled", "SEC,key_digest,0," + DIGEST, "SEC,key_digest,1,none",
                "SEC,key_digest,2,revoked", "SEC,nvs_encryption,on", "SEC,device_key,atecc608b", "SEC,production,yes",
                f"SEC,firmware,{VERSION},{'cd' * 32}"]
        self.assertEqual(ff.security_problems(ff.parse_security(good), DIGEST, VERSION), [])
        bad = [l.replace("key_digest,1,none", "key_digest,1," + "ee" * 32) for l in good if "production" not in l]
        problems = ff.security_problems(ff.parse_security(bad), DIGEST, "0.7.0")
        self.assertEqual(len(problems), 3, problems)


# --- whole units ----------------------------------------------------------------------------

class ProvisionTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.clock = FakeClock()

    def tearDown(self):
        self._tmp.cleanup()

    def run_unit(self, unit, runner, ws=None, camera=None, serial="EVT-0001"):
        ws = ws or Workspace(self.tmp)
        consoles = {"COM7": unit, "COM8": camera}
        record = ff.provision_unit(ws.job(serial, camera=camera is not None), runner=runner,
                                   open_console=lambda port: consoles[port], clock=self.clock, now=StampClock())
        return ws, record

    def manifest(self, ws):
        return [json.loads(l) for l in (ws.records / "manifest.jsonl").read_text().splitlines()]

    def test_a_virgin_unit_end_to_end(self):
        unit = FakeUnit(self.clock)
        runner = FakeRunner(unit, "virgin")
        ws, rec = self.run_unit(unit, runner)
        self.assertEqual(rec["result"], "ok", rec)
        flash = runner.flashed()[0]
        self.assertEqual(flash[flash.index("0x0") + 1], str(ws.boot))
        self.assertEqual(flash[flash.index("0x20000") + 1], str(ws.app))
        self.assertEqual(unit.sent, ["security status", "se status", f"se provision {ff.PROVISION_TOKEN}", "se status",
                                     "ledger key", "pairing"])
        self.assertEqual(unit.boots, 2)  # the first boot, then the restart after provisioning
        self.assertEqual(rec["device_id"], ff.device_id(PUB))
        self.assertEqual(rec["ledger_pub"], PUB.hex())
        self.assertEqual(rec["prev_record_sha256"], ff.ZERO_HASH)
        self.assertEqual(rec["security"]["key_digests"], [DIGEST, "none", "revoked"])
        self.assertEqual(rec["se_config_crc"], ff.SeConfig(bytes(provisioned_config())).crc)
        self.assertEqual(rec["mac"], "00:00:00:00:00:00")
        self.assertEqual(rec["pairing_code_sha256"], hashlib.sha256(PAIRING.encode()).hexdigest())
        manifest_text = (ws.records / "manifest.jsonl").read_text()
        self.assertNotIn(PAIRING, manifest_text)
        self.assertNotIn("7K3QD-9M2XA", manifest_text)
        labels = (ws.records / "labels.csv").read_text().splitlines()
        self.assertEqual(labels[1], f"EVT-0001,{ff.device_id(PUB)},7K3QD-9M2XA")
        log = (ws.records / "EVT-0001" / "console.log").read_text()
        self.assertIn("> se provision", log)
        self.assertTrue((ws.records / "EVT-0001" / "espefuse.json").exists())
        self.assertEqual(ff.verify_manifest((ws.records / "manifest.jsonl").read_bytes()).ok, 1)
        self.assertEqual(unit.closed, 1)

    def test_a_secured_unit_resumes_at_the_console_without_flashing(self):
        unit = FakeUnit(self.clock, secured=True)
        runner = FakeRunner(unit, "secured")
        _, rec = self.run_unit(unit, runner)
        self.assertEqual(rec["result"], "ok", rec)
        self.assertEqual(runner.flashed(), [])
        self.assertEqual(unit.resets, 1)

    def test_an_already_provisioned_unit_is_not_provisioned_again(self):
        unit = FakeUnit(self.clock, secured=True, cfg=provisioned_config())
        _, rec = self.run_unit(unit, FakeRunner(unit, "secured"))
        self.assertEqual(rec["result"], "ok", rec)
        self.assertNotIn(f"se provision {ff.PROVISION_TOKEN}", unit.sent)

    def test_secure_download_mode_resumes_only_if_the_console_proves_the_key(self):
        unit = FakeUnit(self.clock, secured=True)
        _, rec = self.run_unit(unit, FakeRunner(unit, "unreachable"))
        self.assertEqual(rec["result"], "ok", rec)
        self.assertIsNone(rec["mac"])
        unit2 = FakeUnit(self.clock, secured=True, digests=["ee" * 32, "none", "none"])
        (self.tmp / "second").mkdir()
        _, rec2 = self.run_unit(unit2, FakeRunner(unit2, "unreachable"), Workspace(self.tmp / "second"))
        self.assertEqual((rec2["result"], rec2["step"]), ("failed", "security"))

    def test_a_chip_secured_with_another_key_is_refused_before_anything_is_written(self):
        unit = FakeUnit(self.clock, secured=True)
        runner = FakeRunner(unit, "foreign")
        ws, rec = self.run_unit(unit, runner)
        self.assertEqual((rec["result"], rec["step"]), ("failed", "preflight"))
        self.assertIn("another key", rec["reason"])
        self.assertEqual(runner.flashed(), [])
        self.assertEqual(unit.sent, [])
        self.assertFalse((ws.records / "labels.csv").exists())
        self.assertEqual(ff.verify_manifest((ws.records / "manifest.jsonl").read_bytes()).failed, 1)

    def test_images_signed_with_another_key_are_refused(self):
        unit = FakeUnit(self.clock)
        runner = FakeRunner(unit)
        ws = Workspace(self.tmp, app_key=OTHER_KEY)
        _, rec = self.run_unit(unit, runner, ws)
        self.assertEqual((rec["result"], rec["step"]), ("failed", "images"))
        self.assertEqual(runner.calls, [])

    def test_a_node_image_of_another_kind_is_never_flashed(self):
        for n, kind in enumerate(["evt", "devkit", None]):
            with self.subTest(kind):
                d = self.tmp / f"k{n}"
                d.mkdir()
                unit = FakeUnit(self.clock)
                runner = FakeRunner(unit)
                _, rec = self.run_unit(unit, runner, Workspace(d, kind=kind))
                self.assertEqual((rec["result"], rec["step"]), ("failed", "images"))
                self.assertIn(f"image kind {kind or 'none'}", rec["reason"])
                self.assertEqual(runner.calls, [])

    def test_a_secure_element_configured_by_someone_else(self):
        cfg = blank_config()
        apply(cfg, "lock_config")
        unit = FakeUnit(self.clock, cfg=cfg)
        _, rec = self.run_unit(unit, FakeRunner(unit))
        self.assertEqual((rec["result"], rec["step"]), ("failed", "secure_element"))
        self.assertIn("FireX did not write", rec["reason"])
        self.assertNotIn(f"se provision {ff.PROVISION_TOKEN}", unit.sent)

    def test_a_step_that_disagrees_with_the_config_zone(self):
        unit = FakeUnit(self.clock, step_override="done")
        _, rec = self.run_unit(unit, FakeRunner(unit))
        self.assertEqual((rec["result"], rec["step"]), ("failed", "secure_element"))
        self.assertIn("config zone says write_config", rec["reason"])

    def test_security_refusals(self):
        cases = [
            ({"version": "0.5.9-evt"}, "the unit runs 0.5.9-evt"),
            ({"security": {"nvs_encryption": "off"}}, "nvs_encryption is off"),
            ({"security": {"flash_encryption": "development"}}, "flash_encryption is development"),
            ({"security": {"production": "no"}}, "production is no"),
        ]
        for n, (kwargs, reason) in enumerate(cases):
            with self.subTest(reason):
                unit = FakeUnit(self.clock, **kwargs)
                (self.tmp / str(n)).mkdir()
                _, rec = self.run_unit(unit, FakeRunner(unit), Workspace(self.tmp / str(n)))
                self.assertEqual((rec["result"], rec["step"]), ("failed", "security"))
                self.assertIn(reason, rec["reason"])
                self.assertNotIn("se status", unit.sent)

    def test_a_failed_se_provision(self):
        unit = FakeUnit(self.clock, provision_fail="lock_config: ATCA_EXECUTION_ERROR")
        _, rec = self.run_unit(unit, FakeRunner(unit))
        self.assertEqual((rec["result"], rec["step"]), ("failed", "secure_element"))
        self.assertIn("ATCA_EXECUTION_ERROR", rec["reason"])

    def test_a_ledger_key_that_is_not_the_secure_elements(self):
        unit = FakeUnit(self.clock, pem_pub=b"\x04" + b"\x09" * 64)
        _, rec = self.run_unit(unit, FakeRunner(unit))
        self.assertEqual((rec["result"], rec["step"]), ("failed", "identity"))
        self.assertEqual(rec["device_id"], None)

    def test_esptool_failing(self):
        unit = FakeUnit(self.clock)
        _, rec = self.run_unit(unit, FakeRunner(unit, esptool_rc=2))
        self.assertEqual((rec["result"], rec["step"]), ("failed", "flash"))
        self.assertIn("Failed to connect", rec["reason"])

    def test_no_banner_after_flashing(self):
        unit = FakeUnit(self.clock)
        unit.boot = lambda: None  # flashed, but the unit never prints a banner
        _, rec = self.run_unit(unit, FakeRunner(unit))
        self.assertEqual((rec["result"], rec["step"]), ("failed", "first_boot"))
        self.assertGreaterEqual(self.clock.t, ff.FIRST_BOOT_S)

    def test_a_unit_with_its_camera(self):
        unit = FakeUnit(self.clock)
        cam = FakeCamera(self.clock)
        runner = FakeRunner(unit, camera=cam)
        _, rec = self.run_unit(unit, runner, camera=cam)
        self.assertEqual(rec["result"], "ok", rec)
        self.assertEqual(rec["camera"]["version"], CAM_VERSION)
        self.assertTrue(rec["camera"]["flashed"])
        cam_flash = runner.flashed("esp32")[0]
        self.assertEqual(cam_flash[cam_flash.index("0x1000") + 1], str(Path(self.tmp) / "cam-bootloader-signed.bin"))
        self.assertEqual(unit.sent[-1], "cam ping")

    def test_a_camera_that_is_already_secured_and_running_the_image(self):
        unit = FakeUnit(self.clock)
        cam = FakeCamera(self.clock, running=True)
        _, rec = self.run_unit(unit, FakeRunner(unit, camera=cam, camera_rc=2), camera=cam)
        self.assertEqual(rec["result"], "ok", rec)
        self.assertFalse(rec["camera"]["flashed"])

    def test_camera_refusals(self):
        for n, (cam_kwargs, unit_kwargs, reason) in enumerate([
            ({"kind": "bench"}, {}, "bench image"),
            ({"version": "0.1.0-m1"}, {}, "the camera runs 0.1.0-m1"),
            ({}, {"camera_answers": False}, "cannot reach the camera"),
        ]):
            with self.subTest(reason):
                d = self.tmp / f"c{n}"
                d.mkdir()
                unit = FakeUnit(self.clock, **unit_kwargs)
                cam = FakeCamera(self.clock, **cam_kwargs)
                _, rec = self.run_unit(unit, FakeRunner(unit, camera=cam), Workspace(d), camera=cam)
                self.assertEqual((rec["result"], rec["step"]), ("failed", "camera"))
                self.assertIn(reason, rec["reason"])
                self.assertFalse((d / "records" / "labels.csv").exists())


class ManifestTest(unittest.TestCase):
    def build(self, tmp: Path) -> Path:
        path = tmp / "manifest.jsonl"
        base = {k: "" for k in ff.OK_FIELDS if k != "prev_record_sha256"}
        base.update(schema=1, result="ok", security={}, camera=None)
        for n, pub in enumerate([PUB, b"\x04" + b"\x07" * 64]):
            ff.append_record(path, dict(base, unit_serial=f"EVT-000{n + 1}", ledger_pub=pub.hex(),
                                        device_id=ff.device_id(pub)))
        failed = {k: "" for k in ff.FAILED_FIELDS if k != "prev_record_sha256"}
        ff.append_record(path, dict(failed, schema=1, result="failed", unit_serial="EVT-0003"))
        return path

    def test_a_good_chain(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = ff.verify_manifest(self.build(Path(tmp)).read_bytes())
        self.assertEqual((report.ok, report.failed, report.error), (2, 1, None))

    def test_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            lines = self.build(Path(tmp)).read_bytes().split(b"\n")[:-1]
        edited = [lines[0].replace(b'"station":""', b'"station":"x"')] + lines[1:]
        self.assertEqual(ff.verify_manifest(b"\n".join(edited) + b"\n").error[0], 2)
        self.assertEqual(ff.verify_manifest(b"\n".join(lines[1:]) + b"\n").error[0], 1)
        self.assertEqual(ff.verify_manifest(b"\n".join([lines[1], lines[0]]) + b"\n").error[0], 1)
        rec = json.loads(lines[0])
        rec["device_id"] = "00" * 16
        self.assertIn("does not belong", ff.verify_manifest(ff.record_line(rec) + b"\n").error[1])
        self.assertEqual(ff.verify_manifest(b"{not json\n").error, (1, "not a JSON record"))

    def test_the_same_unit_twice(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.jsonl"
            base = {k: "" for k in ff.OK_FIELDS if k != "prev_record_sha256"}
            base.update(schema=1, result="ok", unit_serial="EVT-0001", ledger_pub=PUB.hex(), device_id=ff.device_id(PUB))
            ff.append_record(path, base)
            ff.append_record(path, base)
            self.assertIn("already provisioned", ff.verify_manifest(path.read_bytes()).error[1])


class CliTest(unittest.TestCase):
    def test_dry_run_prints_the_plan_and_touches_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            ws = Workspace(Path(tmp))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = provision.main(["unit", "--port", "COM7", "--build", str(ws.build), "--signed-bootloader",
                                       str(ws.boot), "--signed-app", str(ws.app), "--release-key-digest", DIGEST,
                                       "--unit-serial", "EVT-0001", "--records", str(ws.records), "--dry-run"])
            self.assertEqual(code, 0)
            self.assertFalse(ws.records.exists())
        text = out.getvalue()
        self.assertIn("0x020000", text)
        self.assertIn("--after hard_reset", text)
        self.assertIn(VERSION, text)

    def test_dry_run_refuses_bad_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            ws = Workspace(Path(tmp), app_key=OTHER_KEY)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = provision.main(["unit", "--port", "COM7", "--build", str(ws.build), "--signed-bootloader",
                                       str(ws.boot), "--signed-app", str(ws.app), "--release-key-digest", DIGEST,
                                       "--unit-serial", "EVT-0001", "--dry-run"])
        self.assertEqual(code, 1)
        self.assertIn("not the release key", err.getvalue())

    def test_verify_manifest_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = ManifestTest().build(Path(tmp))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(provision.main(["verify-manifest", str(path)]), 0)
            path.write_bytes(path.read_bytes().replace(b"EVT-0001", b"EVT-0009"))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(provision.main(["verify-manifest", str(path)]), 1)


if __name__ == "__main__":
    unittest.main()
