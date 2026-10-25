#!/usr/bin/env python3
"""
Sign FireX node and camera images: over-the-air updates (M5), and the bootloader and app of
a secure boot image (M6).

    python tools/ota/sign.py keygen /keys/firex-release.pem
    python tools/ota/sign.py sign --key /keys/firex-release.pem firmware/build/firex_node.bin
        # writes firmware/build/firex_node-signed.bin
    python tools/ota/sign.py sign --key /keys/firex-release.pem --build firmware/build-evt-production \
           firmware/build-evt-production/bootloader/bootloader.bin
    python tools/ota/sign.py check firmware/build/firex_node-signed.bin
    python tools/ota/sign.py digest firmware/build/firex_node-signed.bin
        # only the key digest: tools/factory/provision.py --release-key-digest

The dev kit image is built with CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT and the RSA scheme. An
update is written only if its secure boot v2 signature block (RSA-3072, RSA-PSS) verifies
with a key whose digest is also in the running image's own signature block. So an unsigned
image can never install an update: flash the signed image over USB once, and every later
image must be signed with the same key. The M6 production image turns on hardware secure
boot v2 with the same key: the ROM checks the signed bootloader against the key digest in
eFuse, and the bootloader checks the app. Both are signed here, offline.

`--build <idf build dir>` reads the flash offsets from that build's flasher_args.json; without
it the hints use the dev kit's table. A secure boot build leaves the bootloader out of
flasher_args.json, so its offset is the chip's fixed one.

The key never goes into the repository. `keygen` and `sign` run espsecure from an ESP-IDF
environment (`. $IDF_PATH/export.sh`); `check` and `digest` need only Python.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

SECTOR = 4096
BLOCK = 1216
MAGIC = 0xE7
VERSION = 0x02
KEY_START, KEY_END = 36, 36 + 776  # ets_rsa_pubkey_t: n[384], e, rinv[384], mdash
CRC_AT = 1196
APP_OFFSET = 0x10000  # ota_0 in firmware/partitions.csv (the dev kit)
# Where the ROM loads the second-stage bootloader from; fixed per chip.
BOOTLOADER_OFFSETS = {"esp32": 0x1000, "esp32s2": 0x1000, "esp32s3": 0x0, "esp32c3": 0x0, "esp32c6": 0x0}
# The node's image kind (M6, firmware/main/image_kind.c) in ESP-IDF's custom app description:
# image header (24) + segment header (8) + esp_app_desc_t (256), then "FXKIND1\0" and char[24].
KIND_AT = 0x120
KIND_MAGIC = b"FXKIND1\0"
KIND_LEN = 24


def image_kind(data: bytes) -> str | None:
    """devkit, evt or evt-production for a node image built from M6 on; None without the marker."""
    end = KIND_AT + len(KIND_MAGIC) + KIND_LEN
    if len(data) < end or data[KIND_AT:KIND_AT + len(KIND_MAGIC)] != KIND_MAGIC:
        return None
    raw = data[KIND_AT + len(KIND_MAGIC):end]
    kind = raw.split(b"\0", 1)[0]
    if b"\0" not in raw or not kind:  # unterminated or empty: malformed, as fx_ota_kind_refusal says
        return None
    return kind.decode("ascii", "replace")


@dataclass
class SignatureBlock:
    index: int
    image_digest: str
    key_digest: str
    crc_ok: bool
    digest_ok: bool


def signature_blocks(data: bytes) -> list[SignatureBlock]:
    """The v2 signature blocks in the last sector of a signed image, if any."""
    if len(data) < 2 * SECTOR or len(data) % SECTOR:
        return []
    sector_at = len(data) - SECTOR
    image_digest = hashlib.sha256(data[:sector_at]).hexdigest()
    blocks = []
    for index in range(3):
        block = data[sector_at + index * BLOCK: sector_at + (index + 1) * BLOCK]
        if len(block) < BLOCK or block[0] != MAGIC or block[1] != VERSION:
            break
        crc = int.from_bytes(block[CRC_AT:CRC_AT + 4], "little")
        blocks.append(SignatureBlock(
            index=index,
            image_digest=block[4:36].hex(),
            key_digest=hashlib.sha256(block[KEY_START:KEY_END]).hexdigest(),
            crc_ok=zlib.crc32(block[:CRC_AT]) == crc,
            digest_ok=block[4:36].hex() == image_digest,
        ))
    return blocks


@dataclass
class FlasherArgs:
    """The parts of an ESP-IDF build's flasher_args.json the tools use."""
    chip: str
    files: dict[int, str]  # offset -> file, relative to the build directory
    app_offset: int
    app_file: str
    bootloader_offset: int
    bootloader_file: str | None  # None in a secure boot build, which leaves it out
    partition_table_offset: int | None
    write_flash_args: list[str]
    stub: bool


def load_flasher_args(build: str | Path) -> FlasherArgs:
    path = Path(build)
    if path.is_dir():
        path = path / "flasher_args.json"
    doc = json.loads(path.read_text(encoding="utf-8"))
    extra = doc.get("extra_esptool_args", {})
    chip = extra.get("chip")
    if chip not in BOOTLOADER_OFFSETS:
        raise ValueError(f"{path}: unknown chip {chip!r}")
    files = {int(offset, 16): name for offset, name in doc.get("flash_files", {}).items()}
    app = doc.get("app")
    if not app:
        raise ValueError(f"{path}: no app entry")
    boot = doc.get("bootloader")
    table = doc.get("partition-table")
    return FlasherArgs(
        chip=chip,
        files=files,
        app_offset=int(app["offset"], 16),
        app_file=app["file"],
        bootloader_offset=int(boot["offset"], 16) if boot else BOOTLOADER_OFFSETS[chip],
        bootloader_file=boot["file"] if boot else None,
        partition_table_offset=int(table["offset"], 16) if table else None,
        write_flash_args=list(doc.get("write_flash_args", [])),
        stub=bool(extra.get("stub", True)),
    )


def espsecure() -> list[str]:
    for name in ("espsecure.py", "espsecure"):
        found = shutil.which(name)
        if found:
            return [found]
    return [sys.executable, "-m", "espsecure"]


def run(args: list[str]) -> int:
    print("  $ " + " ".join(args))
    try:
        return subprocess.call(args)
    except FileNotFoundError:
        print("espsecure not found: run from an ESP-IDF environment (. $IDF_PATH/export.sh)", file=sys.stderr)
        return 2


def cmd_keygen(args: argparse.Namespace) -> int:
    key = Path(args.key)
    if key.exists():
        print(f"{key} exists; refusing to overwrite a signing key", file=sys.stderr)
        return 1
    return run(espsecure() + ["generate_signing_key", "--version", "2", "--scheme", "rsa3072", str(key)])


def cmd_check(args: argparse.Namespace) -> int:
    data = Path(args.image).read_bytes()
    blocks = signature_blocks(data)
    if not blocks:
        print(f"{args.image}: no signature block; the node will refuse this image as an update")
        return 1
    ok = True
    for b in blocks:
        good = b.crc_ok and b.digest_ok
        ok = ok and good
        print(f"  block {b.index}: key sha256 {b.key_digest}  crc {'ok' if b.crc_ok else 'BAD'}  "
              f"image digest {'matches' if b.digest_ok else 'DOES NOT MATCH'}")
    print(f"  kind {image_kind(data) or 'none'}")
    print(f"{args.image}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")
    return 0 if ok else 1


def cmd_digest(args: argparse.Namespace) -> int:
    blocks = signature_blocks(Path(args.image).read_bytes())
    if len(blocks) != 1:
        print(f"{args.image}: {len(blocks)} signature blocks; expected exactly one", file=sys.stderr)
        return 1
    block = blocks[0]
    if not (block.crc_ok and block.digest_ok):
        print(f"{args.image}: the signature block does not check (crc or image digest)", file=sys.stderr)
        return 1
    print(block.key_digest)
    return 0


def cmd_sign(args: argparse.Namespace) -> int:
    image = Path(args.image)
    out = Path(args.out) if args.out else image.with_name(image.stem + "-signed" + image.suffix)
    if signature_blocks(image.read_bytes()):
        print(f"{image} is already signed", file=sys.stderr)
        return 1
    code = run(espsecure() + ["sign_data", "--version", "2", "--keyfile", args.key, "--output", str(out), str(image)])
    if code != 0:
        return code
    if cmd_check(argparse.Namespace(image=str(out))) != 0:
        return 1
    bootloader = bool(getattr(args, "bootloader", False)) or "bootloader" in image.name
    build = getattr(args, "build", None)
    if build:
        flasher = load_flasher_args(build)
        chip = flasher.chip
        offset = flasher.bootloader_offset if bootloader else flasher.app_offset
    else:
        chip = "esp32s3"
        offset = BOOTLOADER_OFFSETS[chip] if bootloader else APP_OFFSET
    if bootloader:
        print(f"install over USB:        esptool.py --chip {chip} write_flash {offset:#x} {out}")
        print("a bootloader is never an update; on a production unit use tools/factory/provision.py")
        return 0
    print(f"first install over USB:  esptool.py --chip {chip} write_flash {offset:#x} {out}")
    print(f"as an update:            npm run gateway:cli -- firmware add {out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Sign and check FireX node and camera images.")
    sub = ap.add_subparsers(dest="command", required=True)
    k = sub.add_parser("keygen", help="generate an RSA-3072 secure boot v2 signing key (never commit it)")
    k.add_argument("key")
    s = sub.add_parser("sign", help="append the signature block to a built app or bootloader")
    s.add_argument("image")
    s.add_argument("--key", required=True)
    s.add_argument("--out", help="default: <image>-signed.bin")
    s.add_argument("--bootloader", action="store_true",
                   help="the image is a bootloader (implied when its name contains 'bootloader')")
    s.add_argument("--build", help="an ESP-IDF build directory: take flash offsets from its flasher_args.json")
    c = sub.add_parser("check", help="list an image's signature blocks and the key digest in each")
    c.add_argument("image")
    d = sub.add_parser("digest", help="print the key digest of an image with exactly one good signature block")
    d.add_argument("image")
    args = ap.parse_args()
    return {"keygen": cmd_keygen, "sign": cmd_sign, "check": cmd_check, "digest": cmd_digest}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
