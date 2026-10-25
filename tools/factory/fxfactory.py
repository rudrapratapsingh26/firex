"""
Production provisioning of FireX EVT units (M6): every decision, with no I/O of its own
beyond the unit's record files.

tools/factory/provision.py runs a unit through `provision_unit` with a real serial port,
esptool and espefuse. Here those arrive as an injected console, subprocess runner and
clock, so the whole flow runs in tests with no board.

What it reads from the unit's console (firmware/main/console_security.c):

    FireX node <version>, boot <n> (<reset reason>)          at every boot
    SE,<field>,<value>                                        `se status`, `se provision <token>`
    SEC,<field>,<value>                                       `security status`

The order for one unit:
    images -> preflight (espefuse) -> flash (virgin chips only) -> first boot
    -> security status -> secure element -> identity -> camera -> record

Standard library only.
"""
from __future__ import annotations

import base64
import csv
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Protocol

_OTA = Path(__file__).resolve().parent.parent / "ota"
if str(_OTA) not in sys.path:
    sys.path.insert(0, str(_OTA))
import sign  # noqa: E402  tools/ota/sign.py: signature blocks and flasher_args.json

PROVISION_TOKEN = "FIREX-EVT-LOCK"
MANIFEST_SCHEMA = 1
ZERO_HASH = "0" * 64
NODE_CHIP = "esp32s3"
CAMERA_CHIP = "esp32"
NODE_IMAGE_KIND = "evt-production"  # the only node image a factory unit may receive

FIRST_BOOT_S = 240.0   # the bootloader encrypts flash in place on the first boot
RESTART_S = 60.0
COMMAND_S = 20.0
PROVISION_S = 60.0
GRACE_S = 1.0          # lines still arriving after a command's last expected line

# --- the ATECC608B configuration zone (docs/adr/0008-secure-element.md) -------------------

SE_CONFIG_SIZE = 128
LEDGER_SLOT = 0
TLS_SLOT = 1
SLOT_CONFIG_REQUIRED = 0x2081  # external signatures only, secret, GenKey allowed, no PrivWrite
KEY_CONFIG_REQUIRED = 0x0033   # private P-256 key, public info, lockable
UNLOCKED = 0x55
STEPS = ("write_config", "lock_config", "lock_data", "genkey_ledger", "lock_ledger_slot", "done", "refuse")


def atcrc(data: bytes) -> bytes:
    """cryptoauthlib's atCRC: CRC-16 polynomial 0x8005, input bits LSB first, initial 0.
    Returns the two bytes in the order the chip sends them (crc_le)."""
    crc = 0
    for byte in data:
        for bit in range(8):
            data_bit = (byte >> bit) & 1
            crc_bit = crc >> 15
            crc = (crc << 1) & 0xFFFF
            if data_bit != crc_bit:
                crc ^= 0x8005
    return bytes((crc & 0xFF, crc >> 8))


@dataclass(frozen=True)
class SeConfig:
    raw: bytes

    @classmethod
    def from_hex(cls, text: str) -> "SeConfig":
        try:
            raw = bytes.fromhex(text.strip())
        except ValueError as exc:
            raise ValueError(f"SE config is not hex: {exc}") from None
        if len(raw) != SE_CONFIG_SIZE:
            raise ValueError(f"SE config is {len(raw)} bytes, expected {SE_CONFIG_SIZE}")
        return cls(raw)

    def slot_config(self, slot: int) -> int:
        return int.from_bytes(self.raw[20 + 2 * slot:22 + 2 * slot], "little")

    def key_config(self, slot: int) -> int:
        return int.from_bytes(self.raw[96 + 2 * slot:98 + 2 * slot], "little")

    @property
    def data_locked(self) -> bool:
        return self.raw[86] != UNLOCKED

    @property
    def config_locked(self) -> bool:
        return self.raw[87] != UNLOCKED

    def slot_locked(self, slot: int) -> bool:
        return not (int.from_bytes(self.raw[88:90], "little") >> slot) & 1

    @property
    def firex_slots(self) -> bool:
        return all(self.slot_config(s) == SLOT_CONFIG_REQUIRED and self.key_config(s) == KEY_CONFIG_REQUIRED
                   for s in (LEDGER_SLOT, TLS_SLOT))

    @property
    def crc(self) -> str:
        """The summary CRC the config zone is locked with, as a little-endian u16 in hex."""
        return f"{int.from_bytes(atcrc(self.raw), 'little'):04x}"

    @property
    def serial(self) -> str:
        return (self.raw[0:4] + self.raw[8:13]).hex()


def se_config_apply(raw: bytes) -> bytes:
    """firmware fx_se_config_apply: FireX's SlotConfig and KeyConfig for slots 0 and 1, every
    other byte as read off the chip."""
    cfg = bytearray(raw)
    for slot in (LEDGER_SLOT, TLS_SLOT):
        cfg[20 + 2 * slot:22 + 2 * slot] = SLOT_CONFIG_REQUIRED.to_bytes(2, "little")
        cfg[96 + 2 * slot:98 + 2 * slot] = KEY_CONFIG_REQUIRED.to_bytes(2, "little")
    return bytes(cfg)


def se_next_step(cfg: SeConfig) -> tuple[str, str | None]:
    """firmware fx_se_next_step (components/secure/src/se_config.c, the reference): the next
    provisioning step for a chip in this state, or refuse with the firmware's reason. Never
    lock_ledger_slot: a key counts only once its slot is locked, so an unlocked slot 0 reads
    as "generate", and an interrupted GenKey is redone."""
    if not cfg.config_locked:
        if cfg.data_locked:
            return "refuse", "the data zone is locked but the configuration zone is not"
        if cfg.slot_locked(LEDGER_SLOT):
            return "refuse", "slot 0 is marked locked in an unlocked configuration: GenKey could never run"
        return ("lock_config" if cfg.firex_slots else "write_config"), None
    if not cfg.firex_slots:
        return "refuse", "the configuration zone is locked with a slot 0 or 1 configuration FireX did not write"
    if not cfg.data_locked:
        if cfg.slot_locked(LEDGER_SLOT):
            return "refuse", "slot 0 is locked before the data zone: it can never hold a key"
        return "lock_data", None
    if not cfg.slot_locked(LEDGER_SLOT):
        return "genkey_ledger", None
    return "done", None


# --- console lines ------------------------------------------------------------------------

_PROMPT = re.compile(r"^(?:\s*firex>\s*)+")
_BANNER = re.compile(r"FireX node (\S+), boot (\d+) \(([^)]*)\)")
_FXREADY = re.compile(r"FXREADY firex-cam (\S+) (\S+) camera=(\S+)")
_HEX = re.compile(r"^[0-9a-f]*$")


def clean(line: str) -> str:
    return _PROMPT.sub("", line.rstrip("\r\n")).strip()


@dataclass
class Banner:
    version: str
    boot: int
    reset_reason: str


def parse_banner(line: str) -> Banner | None:
    m = _BANNER.search(clean(line))
    return Banner(m.group(1), int(m.group(2)), m.group(3)) if m else None


def _hex_field(name: str, value: str, length: int) -> str:
    value = value.strip().lower()
    if len(value) != length or not _HEX.match(value):
        raise ValueError(f"{name} must be {length} hex characters, got {value!r}")
    return value


@dataclass
class SeStatus:
    backend: str | None = None
    present: bool | None = None
    serial: str | None = None
    revision: str | None = None
    config: SeConfig | None = None
    step: str | None = None
    why: str | None = None
    ledger_pub: bytes | None = None
    steps: list[str] = field(default_factory=list)
    result: str | None = None         # ok | fail
    result_detail: str | None = None  # "already" after ok, the reason after fail


def parse_se(lines: list[str]) -> SeStatus:
    st = SeStatus()
    for raw in lines:
        line = clean(raw)
        if not line.startswith("SE,"):
            continue
        _, name, value = (line.split(",", 2) + [""])[:3]
        if name == "backend":
            st.backend = value
        elif name == "present":
            st.present = value == "yes"
        elif name == "serial":
            st.serial = _hex_field("SE,serial", value, 18)
        elif name == "revision":
            st.revision = _hex_field("SE,revision", value, 8)
        elif name == "config":
            st.config = SeConfig.from_hex(value)
        elif name == "step":
            if value not in STEPS:
                raise ValueError(f"unknown SE step {value!r}")
            st.step = value
            st.steps.append(value)
        elif name == "why":
            st.why = value
        elif name == "ledger_pub":
            pub = bytes.fromhex(_hex_field("SE,ledger_pub", value, 130))
            if pub[0] != 0x04:
                raise ValueError("SE,ledger_pub is not an uncompressed P-256 point")
            st.ledger_pub = pub
        elif name == "result":
            st.result, _, detail = value.partition(",")
            st.result_detail = detail or None
    return st


SECURITY_FIELDS = ("secure_boot", "flash_encryption", "jtag", "usb_jtag", "rom_download", "nvs_encryption",
                   "device_key", "production")
SECURITY_REQUIRED = {
    "secure_boot": ("on",),
    "flash_encryption": ("release",),
    "jtag": ("disabled",),
    "usb_jtag": ("disabled",),
    "rom_download": ("secure", "disabled"),
    "nvs_encryption": ("on",),
    "device_key": ("atecc608b",),
    "production": ("yes",),
}


@dataclass
class SecurityStatus:
    fields: dict[str, str] = field(default_factory=dict)
    key_digests: dict[int, str] = field(default_factory=dict)  # slot -> 64 hex | none | revoked
    firmware_version: str | None = None
    firmware_sha256: str | None = None

    def record(self) -> dict:
        out = {name: self.fields.get(name) for name in SECURITY_FIELDS}
        out["key_digests"] = [self.key_digests.get(slot) for slot in range(3)]
        return out


def parse_security(lines: list[str]) -> SecurityStatus:
    sec = SecurityStatus()
    for raw in lines:
        line = clean(raw)
        if not line.startswith("SEC,"):
            continue
        parts = line.split(",")
        name = parts[1] if len(parts) > 1 else ""
        if name == "key_digest" and len(parts) == 4 and parts[2] in ("0", "1", "2"):
            sec.key_digests[int(parts[2])] = parts[3].lower()
        elif name == "firmware" and len(parts) == 4:
            sec.firmware_version, sec.firmware_sha256 = parts[2], parts[3].lower()
        elif len(parts) == 3:
            sec.fields[name] = parts[2]
    return sec


def security_problems(sec: SecurityStatus, release_key_digest: str, fw_version: str) -> list[str]:
    problems = []
    for name, allowed in SECURITY_REQUIRED.items():
        got = sec.fields.get(name)
        if got is None:
            problems.append(f"security status did not report {name}")
        elif got not in allowed:
            problems.append(f"{name} is {got}, expected {' or '.join(allowed)}")
    digest0 = sec.key_digests.get(0)
    if digest0 != release_key_digest:
        problems.append(f"key digest 0 is {digest0}, expected the release key {release_key_digest}")
    for slot in (1, 2):
        if sec.key_digests.get(slot) not in ("none", "revoked"):
            problems.append(f"key digest {slot} is {sec.key_digests.get(slot)}, expected none or revoked")
    if sec.firmware_version is None:
        problems.append("security status did not report the firmware")
    elif sec.firmware_version != fw_version:
        problems.append(f"the unit runs {sec.firmware_version}, the signed image is {fw_version}")
    if sec.firmware_sha256 is not None and not re.fullmatch(r"[0-9a-f]{64}", sec.firmware_sha256):
        problems.append("the firmware sha256 is not 64 hex characters")
    return problems


# --- identity -----------------------------------------------------------------------------

SPKI_P256_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def pem_public_key(text: str) -> bytes:
    """The 65-byte uncompressed point from a P-256 SPKI PEM."""
    m = re.search(r"-----BEGIN PUBLIC KEY-----(.*?)-----END PUBLIC KEY-----", text, re.S)
    if not m:
        raise ValueError("no PEM public key")
    der = base64.b64decode("".join(m.group(1).split()))
    if len(der) != len(SPKI_P256_PREFIX) + 65 or not der.startswith(SPKI_P256_PREFIX):
        raise ValueError("the PEM is not an uncompressed P-256 public key")
    return der[len(SPKI_P256_PREFIX):]


def device_id(pub: bytes) -> str:
    """ledger/src/entry.ts deviceIdFor: the first 16 bytes of SHA-256 over the SPKI DER."""
    return hashlib.sha256(SPKI_P256_PREFIX + pub).digest()[:16].hex()


def normalise_pairing_code(text: str) -> str | None:
    """gateway/server/pairing.ts normalisePairingCode."""
    s = re.sub(r"[\s-]", "", text.upper()).replace("O", "0").replace("I", "1").replace("L", "1")
    return s if len(s) == 10 and all(c in CROCKFORD for c in s) else None


def format_pairing_code(code: str) -> str:
    return f"{code[:5]}-{code[5:]}"


def parse_pairing(lines: list[str]) -> str | None:
    for raw in lines:
        m = re.search(r"pairing code ([0-9A-Za-z-]{10,11})\b", clean(raw))
        if m:
            return normalise_pairing_code(m.group(1))
    return None


# --- eFuses (espefuse summary --format json) ------------------------------------------------

@dataclass
class EfuseState:
    secure_boot: bool
    flash_encryption: bool
    jtag_disabled: bool
    usb_jtag_disabled: bool
    rom_download: str  # open | secure | disabled
    digests: dict[int, str]  # secure boot digest slot -> 64 hex
    revoked: dict[int, bool]
    mac: str | None


def parse_espefuse(summary: dict) -> EfuseState:
    def value(name: str):
        if name not in summary:
            raise ValueError(f"espefuse summary has no {name}")
        return summary[name]["value"]

    crypt = summary.get("SPI_BOOT_CRYPT_CNT", {})
    if "raw_value" in crypt:
        flash_encryption = bin(int(str(crypt["raw_value"]), 0)).count("1") % 2 == 1
    else:
        flash_encryption = value("SPI_BOOT_CRYPT_CNT") == "Enable"
    if value("DIS_DOWNLOAD_MODE"):
        rom = "disabled"
    elif value("ENABLE_SECURITY_DOWNLOAD"):
        rom = "secure"
    else:
        rom = "open"
    digests = {}
    for n in range(6):
        m = re.fullmatch(r"SECURE_BOOT_DIGEST(\d)", str(value(f"KEY_PURPOSE_{n}")))
        if m:
            digests[int(m.group(1))] = str(value(f"BLOCK_KEY{n}")).replace(" ", "").lower()
    revoked = {k: bool(value(f"SECURE_BOOT_KEY_REVOKE{k}")) for k in range(3)}
    mac = str(summary.get("MAC", {}).get("value", "")).split(" ")[0] or None
    return EfuseState(
        secure_boot=bool(value("SECURE_BOOT_EN")),
        flash_encryption=flash_encryption,
        jtag_disabled=bool(value("DIS_PAD_JTAG")),
        usb_jtag_disabled=bool(value("DIS_USB_JTAG")),
        rom_download=rom,
        digests=digests,
        revoked=revoked,
        mac=mac,
    )


def classify_chip(state: EfuseState, release_key_digest: str) -> tuple[str, str | None]:
    """virgin (flash it), ours (already secured with the release key: resume at the console),
    or refuse, with the reason."""
    live = {slot: d for slot, d in state.digests.items() if not state.revoked.get(slot)}
    foreign = [d for d in live.values() if d != release_key_digest]
    if state.secure_boot:
        if not live:
            return "refuse", "secure boot is on with no usable key digest"
        if foreign:
            return "refuse", f"secure boot is on with another key (digest {foreign[0]})"
        if not state.flash_encryption:
            return "refuse", "secure boot is on but flash encryption is off"
        return "ours", None
    if state.flash_encryption:
        return "refuse", "flash encryption is on but secure boot is off: not a FireX first boot"
    if foreign:
        return "refuse", f"a key digest from another key is already burned ({foreign[0]})"
    return "virgin", None


# --- images and the flash plan ------------------------------------------------------------

APP_DESC_MAGIC = 0xABCD5432
IMAGE_MAGIC = 0xE9


@dataclass
class ImageInfo:
    path: str
    kind: str  # app | bootloader
    sha256: str
    key_digest: str | None
    project: str | None = None
    version: str | None = None
    image_kind: str | None = None


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def app_description(data: bytes) -> tuple[str, str] | None:
    """(project, version) from esp_app_desc_t, which follows the image and first segment
    headers (24 + 8 bytes): magic, secure_version, reserved[2], version[32], project_name[32]."""
    if len(data) < 0x70 or data[0] != IMAGE_MAGIC or int.from_bytes(data[0x20:0x24], "little") != APP_DESC_MAGIC:
        return None
    return _cstr(data[0x50:0x70]), _cstr(data[0x30:0x50])


def inspect_signed_image(path: str | Path, data: bytes, kind: str, release_key_digest: str,
                         project: str | None = None, image_kind: str | None = None) -> tuple[ImageInfo, list[str]]:
    """`image_kind`, when given, is the kind marker the app must carry (sign.image_kind)."""
    problems = []
    info = ImageInfo(str(path), kind, hashlib.sha256(data).hexdigest(), None, image_kind=sign.image_kind(data))
    if image_kind and info.image_kind != image_kind:
        problems.append(f"{path}: image kind {info.image_kind or 'none'}, expected {image_kind}")
    if not data or data[0] != IMAGE_MAGIC:
        problems.append(f"{path}: not an ESP-IDF image")
    blocks = sign.signature_blocks(data)
    if len(blocks) != 1:
        problems.append(f"{path}: {len(blocks)} signature blocks, expected exactly one")
    else:
        block = blocks[0]
        info.key_digest = block.key_digest
        if not block.crc_ok or not block.digest_ok:
            problems.append(f"{path}: the signature block does not check")
        if block.key_digest != release_key_digest:
            problems.append(f"{path}: signed with key {block.key_digest}, not the release key")
    if kind == "app":
        desc = app_description(data)
        if not desc:
            problems.append(f"{path}: no app description: not an app image")
        else:
            info.project, info.version = desc
            if project and info.project != project:
                problems.append(f"{path}: an image of {info.project}, expected {project}")
    return info, problems


@dataclass
class FlashPlan:
    chip: str
    entries: list[tuple[int, str]]
    write_flash_args: list[str]
    stub: bool

    def command(self, python: str, port: str) -> list[str]:
        args = [python, "-m", "esptool", "--chip", self.chip, "-p", port,
                "--before", "default_reset", "--after", "hard_reset"]
        if not self.stub:
            args.append("--no-stub")
        args += ["write_flash", *self.write_flash_args]
        for offset, path in sorted(self.entries):
            args += [f"{offset:#x}", path]
        return args


def flash_plan(build_dir: str | Path, signed_bootloader: str | Path, signed_app: str | Path, chip: str) -> FlashPlan:
    """Everything the build would flash, with the signed bootloader and app in place of its own.
    A secure boot build leaves the bootloader out of flasher_args.json; it goes at the chip's
    fixed offset. The partition table offset always comes from the build."""
    fa = sign.load_flasher_args(build_dir)
    if fa.chip != chip:
        raise ValueError(f"{build_dir} is a build for {fa.chip}, expected {chip}")
    if fa.partition_table_offset is None:
        raise ValueError(f"{build_dir}: flasher_args.json has no partition table")
    build = Path(build_dir)
    entries = {}
    for offset, name in fa.files.items():
        if offset in (fa.app_offset, fa.bootloader_offset) or name in (fa.app_file, fa.bootloader_file):
            continue
        entries[offset] = str(build / name)
    entries[fa.bootloader_offset] = str(signed_bootloader)
    entries[fa.app_offset] = str(signed_app)
    return FlashPlan(chip, sorted(entries.items()), fa.write_flash_args, fa.stub)


# --- the manifest -------------------------------------------------------------------------

OK_FIELDS = ("schema", "result", "unit_serial", "chip", "mac", "se_serial", "se_revision", "device_id", "ledger_pub",
             "fw_version", "fw_sha256", "image_sha256", "release_key_digest", "security", "se_config_crc", "camera",
             "pairing_code_sha256", "operator", "station", "started_at", "finished_at", "prev_record_sha256")
FAILED_FIELDS = ("schema", "result", "unit_serial", "chip", "release_key_digest", "step", "reason", "operator",
                 "station", "started_at", "finished_at", "prev_record_sha256")


def record_line(record: dict) -> bytes:
    return json.dumps(record, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def last_record_hash(manifest: Path) -> str:
    if not manifest.exists():
        return ZERO_HASH
    lines = [line for line in manifest.read_bytes().split(b"\n") if line.strip()]
    return hashlib.sha256(lines[-1]).hexdigest() if lines else ZERO_HASH


def append_record(manifest: Path, record: dict) -> dict:
    """Chain the record to the previous line (sha256 of that line's bytes, without its newline)."""
    manifest.parent.mkdir(parents=True, exist_ok=True)
    record = dict(record, prev_record_sha256=last_record_hash(manifest))
    with manifest.open("ab") as f:
        f.write(record_line(record) + b"\n")
    return record


@dataclass
class ManifestReport:
    ok: int = 0
    failed: int = 0
    error: tuple[int, str] | None = None  # (line number, reason)


def verify_manifest(data: bytes) -> ManifestReport:
    report = ManifestReport()
    prev = ZERO_HASH
    serials: set[str] = set()
    ids: set[str] = set()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()

    def fail(n: int, reason: str) -> ManifestReport:
        report.error = (n, reason)
        return report

    for n, line in enumerate(lines, start=1):
        try:
            rec = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            return fail(n, "not a JSON record")
        if not isinstance(rec, dict) or rec.get("schema") != MANIFEST_SCHEMA:
            return fail(n, f"schema is not {MANIFEST_SCHEMA}")
        if rec.get("prev_record_sha256") != prev:
            return fail(n, "prev_record_sha256 does not match the line before: a record was changed, removed or reordered")
        result = rec.get("result")
        needed = OK_FIELDS if result == "ok" else FAILED_FIELDS if result == "failed" else None
        if needed is None:
            return fail(n, f"unknown result {result!r}")
        missing = [k for k in needed if k not in rec]
        if missing:
            return fail(n, f"missing {', '.join(missing)}")
        if result == "ok":
            try:
                pub = bytes.fromhex(rec["ledger_pub"])
            except (TypeError, ValueError):
                return fail(n, "ledger_pub is not hex")
            if len(pub) != 65 or pub[0] != 0x04:
                return fail(n, "ledger_pub is not an uncompressed P-256 point")
            if device_id(pub) != rec["device_id"]:
                return fail(n, "device_id does not belong to ledger_pub")
            if rec["unit_serial"] in serials:
                return fail(n, f"unit {rec['unit_serial']} was already provisioned")
            if rec["device_id"] in ids:
                return fail(n, f"device {rec['device_id']} was already provisioned")
            serials.add(rec["unit_serial"])
            ids.add(rec["device_id"])
            report.ok += 1
        else:
            report.failed += 1
        prev = hashlib.sha256(line).hexdigest()
    return report


def append_label(labels: Path, unit_serial: str, dev_id: str, code: str) -> None:
    new = not labels.exists() or labels.stat().st_size == 0
    labels.parent.mkdir(parents=True, exist_ok=True)
    with labels.open("a", newline="", encoding="ascii") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["unit_serial", "device_id", "pairing_code"])
        w.writerow([unit_serial, dev_id, format_pairing_code(code)])


# --- running a unit -----------------------------------------------------------------------

class Console(Protocol):
    def write_line(self, text: str) -> None: ...
    def read_line(self, timeout: float) -> str | None: ...
    def reset(self) -> None: ...
    def close(self) -> None: ...


Runner = Callable[[list[str]], "tuple[int, str, str]"]


class FactoryError(Exception):
    def __init__(self, step: str, reason: str):
        super().__init__(f"{step}: {reason}")
        self.step = step
        self.reason = reason


class ConsoleTimeout(Exception):
    pass


class Session:
    def __init__(self, console: Console, clock: Callable[[], float], log: Callable[[str], None]):
        self.console = console
        self.clock = clock
        self.log = log

    def read_until(self, done: Callable[[str], bool], timeout: float) -> list[str]:
        lines: list[str] = []
        deadline = self.clock() + timeout
        while True:
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise ConsoleTimeout(f"no answer within {timeout:.0f} s")
            line = self.console.read_line(min(remaining, 1.0))
            if line is None:
                continue
            self.log(line)
            lines.append(line)
            if done(clean(line)):
                return lines

    def drain(self, grace: float) -> list[str]:
        lines = []
        while (line := self.console.read_line(grace)) is not None:
            self.log(line)
            lines.append(line)
        return lines

    def command(self, text: str, done: Callable[[str], bool], timeout: float = COMMAND_S, grace: bool = True) -> list[str]:
        self.drain(0.0)
        self.log(f"> {text}")
        self.console.write_line(text)
        lines = self.read_until(done, timeout)
        return lines + (self.drain(GRACE_S) if grace else [])

    def wait_banner(self, timeout: float) -> Banner:
        lines = self.read_until(lambda l: parse_banner(l) is not None, timeout)
        return parse_banner(lines[-1])  # type: ignore[return-value]


@dataclass
class CameraJob:
    port: str
    build_dir: str
    signed_bootloader: str
    signed_app: str


@dataclass
class UnitJob:
    port: str
    build_dir: str
    signed_bootloader: str
    signed_app: str
    release_key_digest: str
    unit_serial: str
    records_dir: str
    operator: str = ""
    station: str = ""
    camera: CameraJob | None = None
    python: str = sys.executable


def check_job(job: UnitJob) -> None:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", job.unit_serial):
        raise FactoryError("images", f"unit serial {job.unit_serial!r}: letters, digits, '.', '_' and '-' only")
    if not re.fullmatch(r"[0-9a-f]{64}", job.release_key_digest):
        raise FactoryError("images", "the release key digest must be 64 lower-case hex characters (sign.py digest)")


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


@dataclass
class Images:
    bootloader: ImageInfo
    app: ImageInfo
    plan: FlashPlan
    camera_app: ImageInfo | None = None
    camera_plan: FlashPlan | None = None


def check_images(job: UnitJob) -> Images:
    """Offline: both images signed once with the release key, and flash plans from the builds."""
    problems = []
    reads = {}
    for label, path in [("bootloader", job.signed_bootloader), ("app", job.signed_app)] + (
            [("camera bootloader", job.camera.signed_bootloader), ("camera app", job.camera.signed_app)]
            if job.camera else []):
        try:
            reads[label] = Path(path).read_bytes()
        except OSError as exc:
            problems.append(f"{path}: {exc.strerror or exc}")
    if problems:
        raise FactoryError("images", "; ".join(problems))
    boot, p1 = inspect_signed_image(job.signed_bootloader, reads["bootloader"], "bootloader", job.release_key_digest)
    app, p2 = inspect_signed_image(job.signed_app, reads["app"], "app", job.release_key_digest, "firex_node",
                                   NODE_IMAGE_KIND)
    problems += p1 + p2
    cam_app = cam_plan = None
    if job.camera:
        _, p3 = inspect_signed_image(job.camera.signed_bootloader, reads["camera bootloader"], "bootloader",
                                     job.release_key_digest)
        cam_app, p4 = inspect_signed_image(job.camera.signed_app, reads["camera app"], "app", job.release_key_digest,
                                           "firex_cam")
        problems += p3 + p4
    try:
        plan = flash_plan(job.build_dir, job.signed_bootloader, job.signed_app, NODE_CHIP)
        if job.camera:
            cam_plan = flash_plan(job.camera.build_dir, job.camera.signed_bootloader, job.camera.signed_app, CAMERA_CHIP)
    except (OSError, ValueError, KeyError) as exc:
        problems.append(f"flash plan: {exc}")
    if problems:
        raise FactoryError("images", "; ".join(problems))
    return Images(boot, app, plan, cam_app, cam_plan)


def plan_unit(job: UnitJob) -> list[str]:
    """What `unit` would do, for --dry-run. Reads the images and builds; touches no port."""
    check_job(job)
    images = check_images(job)
    unit_dir = Path(job.records_dir) / job.unit_serial
    out = [
        f"unit {job.unit_serial} on {job.port}",
        f"  app {images.app.version} ({images.app.project}, kind {images.app.image_kind}), sha256 {images.app.sha256}",
        f"  release key digest {job.release_key_digest}",
        "1. preflight: " + " ".join(espefuse_command(job.python, NODE_CHIP, job.port, unit_dir / "espefuse.json")),
        "   virgin -> flash; secure boot with the release key -> resume at the console; anything else -> refuse",
        "2. flash (virgin chips only):",
    ]
    out += [f"     {offset:#08x}  {path}" for offset, path in images.plan.entries]
    out.append("   $ " + " ".join(images.plan.command(job.python, job.port)))
    out += [
        f"3. first boot: wait up to {FIRST_BOOT_S:.0f} s for 'FireX node {images.app.version}, boot ...'",
        "4. security status: " + ", ".join(f"{k} {'|'.join(v)}" for k, v in SECURITY_REQUIRED.items())
        + f", key digest 0 = release key, 1-2 none|revoked, firmware {images.app.version}",
        f"5. se status; se provision {PROVISION_TOKEN} until done (the unit restarts once); step checked against the config zone",
        "6. ledger key and pairing: device_id from the PEM must match SE,ledger_pub",
    ]
    if job.camera and images.camera_plan and images.camera_app:
        out.append(f"7. camera on {job.camera.port}: {images.camera_app.version}")
        out += [f"     {offset:#08x}  {path}" for offset, path in images.camera_plan.entries]
        out.append("   $ " + " ".join(images.camera_plan.command(job.python, job.camera.port)))
        out.append(f"   wait for 'FXREADY firex-cam {images.camera_app.version} link-only camera=ok', then `cam ping` on the node")
    out += [
        f"record: {Path(job.records_dir) / 'manifest.jsonl'} (chained), console log {unit_dir / 'console.log'}",
        f"label:  {Path(job.records_dir) / 'labels.csv'} (holds pairing codes: destroy it once the labels are printed)",
    ]
    return out


def espefuse_command(python: str, chip: str, port: str, out: Path) -> list[str]:
    return [python, "-m", "espefuse", "--chip", chip, "-p", port, "summary", "--format", "json", "--file", str(out)]


def provision_unit(job: UnitJob, *, runner: Runner, open_console: Callable[[str], Console],
                   clock: Callable[[], float], now: Callable[[], str] = now_iso,
                   echo: Callable[[str], None] = lambda s: None) -> dict:
    """Run one unit. Returns the record appended to the manifest, ok or failed."""
    started = now()
    records = Path(job.records_dir)
    manifest = records / "manifest.jsonl"
    step = "images"
    known: dict = {"mac": None, "device_id": None}
    unit_dir = records / job.unit_serial
    try:
        check_job(job)
        unit_dir.mkdir(parents=True, exist_ok=True)
        with (unit_dir / "console.log").open("a", encoding="utf-8", errors="replace") as logf:
            def log(line: str) -> None:
                logf.write(line.rstrip("\r\n") + "\n")
                logf.flush()
                echo(line)

            log(f"# {started} unit {job.unit_serial} station {job.station} operator {job.operator}")
            images = check_images(job)
            fw_version = images.app.version or ""

            step = "preflight"
            efuse_json = unit_dir / "espefuse.json"
            rc, _, err = runner(espefuse_command(job.python, NODE_CHIP, job.port, efuse_json))
            state = None
            if rc == 0 and efuse_json.exists():
                try:
                    state = parse_espefuse(json.loads(efuse_json.read_text(encoding="utf-8")))
                except (ValueError, KeyError) as exc:
                    raise FactoryError(step, f"espefuse summary could not be read: {exc}") from None
            if state:
                known["mac"] = state.mac
                kind, why = classify_chip(state, job.release_key_digest)
                if kind == "refuse":
                    raise FactoryError(step, why or "refused")
            else:
                kind = "unknown"
                log("# espefuse could not read the chip (secure download mode?): resuming at the console; "
                    "only the release key digest will be accepted")

            if kind == "virgin":
                step = "flash"
                rc, out, err = runner(images.plan.command(job.python, job.port))
                if rc != 0:
                    tail = (err or out).strip().splitlines()[-1:] or ["no output"]
                    raise FactoryError(step, f"esptool failed ({rc}): {tail[0]}")

            console = open_console(job.port)
            try:
                s = Session(console, clock, log)
                if kind == "virgin":
                    step = "first_boot"
                    try:
                        s.wait_banner(FIRST_BOOT_S)
                    except ConsoleTimeout:
                        raise FactoryError(step, f"no boot banner within {FIRST_BOOT_S:.0f} s") from None
                else:
                    step = "first_boot"
                    console.reset()
                    try:
                        s.wait_banner(RESTART_S)
                    except ConsoleTimeout:
                        raise FactoryError(step, f"the unit did not boot within {RESTART_S:.0f} s of a reset") from None

                step = "security"
                sec = parse_security(_command(s, step, "security status", lambda l: l.startswith("SEC,firmware,")))
                problems = security_problems(sec, job.release_key_digest, fw_version)
                if problems:
                    raise FactoryError(step, "; ".join(problems))

                step = "secure_element"
                se = _se_status(s, step)
                if se.step != "done":
                    lines = _command(s, step, f"se provision {PROVISION_TOKEN}",
                                     lambda l: l.startswith("SE,result,"), timeout=PROVISION_S, grace=False)
                    result = parse_se(lines)
                    if result.result != "ok":
                        raise FactoryError(step, f"se provision failed: {result.result_detail or result.result}")
                    if result.result_detail != "already":
                        try:
                            s.wait_banner(RESTART_S)
                        except ConsoleTimeout:
                            raise FactoryError(step, "the unit did not restart after provisioning") from None
                    se = _se_status(s, step)
                    if se.step != "done":
                        raise FactoryError(step, f"the secure element is at {se.step} after provisioning")
                if not se.ledger_pub or se.config is None:
                    raise FactoryError(step, "se status reported done without a ledger key")

                step = "identity"
                pem_lines = _command(s, step, "ledger key", lambda l: "-----END PUBLIC KEY-----" in l)
                try:
                    pem_pub = pem_public_key("\n".join(clean(l) for l in pem_lines))
                except ValueError as exc:
                    raise FactoryError(step, f"ledger key: {exc}") from None
                if pem_pub != se.ledger_pub:
                    raise FactoryError(step, "the ledger key PEM is not the secure element's ledger key")
                known["device_id"] = device_id(se.ledger_pub)
                code = parse_pairing(_command(s, step, "pairing", lambda l: l.startswith("pairing code")
                                              or l.startswith("no pairing code")))
                if not code:
                    raise FactoryError(step, "the unit has no valid pairing code")

                camera = None
                if job.camera and images.camera_plan and images.camera_app:
                    step = "camera"
                    camera = _camera(job, images, runner, open_console, clock, log)
                    ping = _command(s, step, "cam ping", lambda l: l.startswith("camera "))
                    if not any("camera answered" in clean(l) for l in ping):
                        raise FactoryError(step, "the node cannot reach the camera over its link")
            finally:
                console.close()

            step = "record"
            record = {
                "schema": MANIFEST_SCHEMA,
                "result": "ok",
                "unit_serial": job.unit_serial,
                "chip": NODE_CHIP,
                "mac": known["mac"],
                "se_serial": se.serial,
                "se_revision": se.revision,
                "device_id": known["device_id"],
                "ledger_pub": se.ledger_pub.hex(),
                "fw_version": sec.firmware_version,
                "fw_sha256": sec.firmware_sha256,
                "image_sha256": images.app.sha256,
                "release_key_digest": job.release_key_digest,
                "security": sec.record(),
                "se_config_crc": se.config.crc,
                "camera": camera,
                "pairing_code_sha256": hashlib.sha256(code.encode("ascii")).hexdigest(),
                "operator": job.operator,
                "station": job.station,
                "started_at": started,
                "finished_at": now(),
            }
            record = append_record(manifest, record)
            append_label(records / "labels.csv", job.unit_serial, known["device_id"], code)
            log(f"# ok: device {known['device_id']}")
            return record
    except FactoryError as exc:
        failed = {
            "schema": MANIFEST_SCHEMA,
            "result": "failed",
            "unit_serial": job.unit_serial,
            "chip": NODE_CHIP,
            "mac": known["mac"],
            "device_id": known["device_id"],
            "release_key_digest": job.release_key_digest,
            "step": exc.step,
            "reason": exc.reason,
            "operator": job.operator,
            "station": job.station,
            "started_at": started,
            "finished_at": now(),
        }
        echo(f"FAILED at {exc.step}: {exc.reason}")
        return append_record(manifest, failed)


def _command(s: Session, step: str, text: str, done: Callable[[str], bool], timeout: float = COMMAND_S,
             grace: bool = True) -> list[str]:
    try:
        return s.command(text, done, timeout, grace)
    except ConsoleTimeout as exc:
        raise FactoryError(step, f"`{text}`: {exc}") from None


def _se_status(s: Session, step: str) -> SeStatus:
    lines = _command(s, step, "se status", lambda l: l.startswith("SE,step,"))
    try:
        se = parse_se(lines)
    except ValueError as exc:
        raise FactoryError(step, f"se status: {exc}") from None
    if se.backend != "atecc608b" or not se.present:
        raise FactoryError(step, f"no ATECC608B (backend {se.backend}, present {se.present})")
    if se.config is None or se.step is None:
        raise FactoryError(step, "se status did not report the config zone and step")
    computed, why = se_next_step(se.config)
    if se.step != computed:
        raise FactoryError(step, f"the unit says {se.step} but its config zone says {computed}")
    if computed == "refuse":
        raise FactoryError(step, se.why or why or "refused")
    if se.serial and se.serial != se.config.serial:
        raise FactoryError(step, "SE,serial is not the serial number in the config zone")
    return se


def _camera(job: UnitJob, images: Images, runner: Runner, open_console: Callable[[str], Console],
            clock: Callable[[], float], log: Callable[[str], None]) -> dict:
    cam = job.camera
    assert cam and images.camera_plan and images.camera_app
    version = images.camera_app.version
    rc, out, err = runner(images.camera_plan.command(job.python, cam.port))
    flashed = rc == 0
    if not flashed:
        tail = (err or out).strip().splitlines()[-1:] or ["no output"]
        log(f"# camera: esptool failed ({rc}: {tail[0]}); checking whether it already runs {version}")
    console = open_console(cam.port)
    try:
        s = Session(console, clock, log)
        if not flashed:
            console.reset()
        try:
            lines = s.read_until(lambda l: _FXREADY.search(l) is not None, FIRST_BOOT_S if flashed else RESTART_S)
        except ConsoleTimeout:
            raise FactoryError("camera", "the camera could not be flashed and did not report FXREADY" if not flashed
                               else "no FXREADY from the camera after flashing") from None
    finally:
        console.close()
    m = _FXREADY.search(clean(lines[-1]))
    assert m
    got_version, kind, status = m.groups()
    if got_version != version:
        raise FactoryError("camera", f"the camera runs {got_version}, the signed image is {version}")
    if kind != "link-only":
        raise FactoryError("camera", f"the camera image is a {kind} image")
    if status != "ok":
        raise FactoryError("camera", f"the camera sensor did not start (camera={status})")
    return {"version": version, "image_sha256": images.camera_app.sha256, "flashed": flashed}
