#!/usr/bin/env python3
"""
Provision FireX EVT units at the factory (M6).

    python tools/factory/provision.py unit --port COM7 \\
        --build firmware/build-evt-production \\
        --signed-bootloader firmware/build-evt-production/bootloader/bootloader-signed.bin \\
        --signed-app firmware/build-evt-production/firex_node-signed.bin \\
        --release-key-digest <python tools/ota/sign.py digest firex_node-signed.bin> \\
        --unit-serial EVT-0001 --records factory-records --operator NAME --station NAME \\
        [--camera-port COM8 --camera-build firmware/cam/build-production \\
         --camera-signed-bootloader ... --camera-signed-app ...] [--dry-run]

    python tools/factory/provision.py verify-manifest factory-records/manifest.jsonl

The first boot of a production image burns eFuses that can never be reset: secure boot with
the release key, flash encryption in release mode, JTAG off, the ROM loader restricted. This
tool flashes only chips whose eFuses are still blank, checks all of that from the unit's own
`security status`, has the unit generate its ledger key inside the ATECC608B, and appends a
chained record to <records>/manifest.jsonl. `--dry-run` prints the plan and touches no port.

<records>/labels.csv holds the pairing codes for the label printer and nothing else does:
destroy it once the labels are printed. The manifest keeps only each code's SHA-256.

Needs pyserial and esptool (tools/factory/requirements.txt).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxfactory as ff  # noqa: E402


class SerialConsole:
    """The unit's UART console. DTR and RTS stay released when the port opens, so opening it
    does not reset the chip; `reset` pulses EN through RTS the way esptool does."""

    def __init__(self, port: str, baud: int = 115200):
        try:
            import serial  # noqa: PLC0415
        except ImportError:
            raise SystemExit("pyserial is missing: pip install -r tools/factory/requirements.txt") from None
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, baud, 0
        s.dtr = False
        s.rts = False
        s.open()
        self._serial = s
        self._buffer = b""

    def write_line(self, text: str) -> None:
        self._serial.write((text.strip() + "\n").encode("ascii", "replace"))

    def read_line(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while True:
            end = self._buffer.find(b"\n")
            if end >= 0:
                line, self._buffer = self._buffer[:end + 1], self._buffer[end + 1:]
                return line.decode("utf-8", "replace").rstrip("\r\n")
            chunk = self._serial.read(self._serial.in_waiting or 1)
            if chunk:
                self._buffer += chunk
                continue
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.01)

    def reset(self) -> None:
        self._serial.dtr = False
        self._serial.rts = True
        time.sleep(0.1)
        self._serial.rts = False
        time.sleep(0.05)

    def close(self) -> None:
        self._serial.close()


def run_subprocess(args: list[str]) -> tuple[int, str, str]:
    print("  $ " + " ".join(args), flush=True)
    try:
        done = subprocess.run(args, capture_output=True, text=True)
    except FileNotFoundError as exc:
        return 127, "", str(exc)
    if done.stdout:
        print(done.stdout.rstrip()[-4000:])
    if done.returncode != 0 and done.stderr:
        print(done.stderr.rstrip()[-4000:], file=sys.stderr)
    return done.returncode, done.stdout, done.stderr


def cmd_unit(args: argparse.Namespace) -> int:
    camera_args = [args.camera_port, args.camera_build, args.camera_signed_bootloader, args.camera_signed_app]
    if any(camera_args) and not all(camera_args):
        print("give all of --camera-port, --camera-build, --camera-signed-bootloader and --camera-signed-app, or none",
              file=sys.stderr)
        return 2
    camera = ff.CameraJob(*camera_args) if all(camera_args) else None
    job = ff.UnitJob(
        port=args.port,
        build_dir=args.build,
        signed_bootloader=args.signed_bootloader,
        signed_app=args.signed_app,
        release_key_digest=args.release_key_digest.lower(),
        unit_serial=args.unit_serial,
        records_dir=args.records,
        operator=args.operator,
        station=args.station,
        camera=camera,
    )
    if args.dry_run:
        try:
            print("\n".join(ff.plan_unit(job)))
        except ff.FactoryError as exc:
            print(f"refused: {exc}", file=sys.stderr)
            return 1
        return 0

    record = ff.provision_unit(job, runner=run_subprocess, open_console=lambda port: SerialConsole(port, args.baud),
                               clock=time.monotonic, echo=print)
    if record["result"] != "ok":
        print(f"\n{job.unit_serial} FAILED at {record['step']}: {record['reason']}", file=sys.stderr)
        print(f"recorded in {Path(job.records_dir) / 'manifest.jsonl'}; no label was written", file=sys.stderr)
        return 1
    print(f"\n{job.unit_serial} provisioned: device {record['device_id']}, firmware {record['fw_version']}")
    print(f"recorded in {Path(job.records_dir) / 'manifest.jsonl'}")
    print(f"WARNING: {Path(job.records_dir) / 'labels.csv'} holds pairing codes. Print the labels, then destroy it.")
    return 0


def cmd_verify_manifest(args: argparse.Namespace) -> int:
    path = Path(args.manifest)
    report = ff.verify_manifest(path.read_bytes())
    if report.error:
        line, reason = report.error
        print(f"{path}:{line}: {reason}", file=sys.stderr)
        return 1
    print(f"{path}: {report.ok} units provisioned, {report.failed} failed attempts, chain intact")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Provision FireX EVT units: flash, secure, key and record.")
    sub = ap.add_subparsers(dest="command", required=True)
    u = sub.add_parser("unit", help="provision one unit")
    u.add_argument("--port", required=True, help="the node's console and ROM loader port")
    u.add_argument("--baud", type=int, default=115200)
    u.add_argument("--build", required=True, help="the ESP-IDF build directory of the evt-production image")
    u.add_argument("--signed-bootloader", required=True)
    u.add_argument("--signed-app", required=True)
    u.add_argument("--release-key-digest", required=True, help="64 hex: python tools/ota/sign.py digest <signed app>")
    u.add_argument("--unit-serial", required=True)
    u.add_argument("--records", default="factory-records")
    u.add_argument("--operator", default="")
    u.add_argument("--station", default="")
    u.add_argument("--camera-port")
    u.add_argument("--camera-build")
    u.add_argument("--camera-signed-bootloader")
    u.add_argument("--camera-signed-app")
    u.add_argument("--dry-run", action="store_true", help="print the plan; touch no port")
    v = sub.add_parser("verify-manifest", help="check a manifest's chain, schema and duplicates")
    v.add_argument("manifest")
    args = ap.parse_args(argv)
    return {"unit": cmd_unit, "verify-manifest": cmd_verify_manifest}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
