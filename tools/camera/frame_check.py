#!/usr/bin/env python3
"""
Camera-in-the-loop check (M4): measure the frames a CRITICAL captured against
frames of the same room in the clear, and flag the ones that are dark, obscured
or blurred.

    pip install -r tools/camera/requirements.txt
    python tools/camera/frame_check.py --reference captures/cam-ref-media \\
        --frames captures/cam-smoke-run1-media captures/cam-unlit-media --out reports/camera

Reference frames: on the bench image, with the room lit and clear, fire a CRITICAL
with `inject flame 8` and export its photos (tools/ledger/export_log.py --media, or
download them from the gateway). They are captured through the normal CRITICAL path,
so no remote shutter is needed and the media policy holds. Keep the camera and the
room exactly as they will be for the trial.

Frames to check: export the photos of each trial the same way, one directory per
trial. Node file names (b<boot>-c<capture>-f<frame>-<hash>.jpg) give each frame its
index, so the table shows whether the first frame of an incident is the useful one.

Writes <out>.json for tools/validation/report.py and <out>.md to read. The
measures are defined in tools/camera/fxframes.py.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxframes  # noqa: E402

IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png"}
NODE_NAME = re.compile(r"-c(\d+)-f(\d+)-")


def load_luma(path: Path) -> tuple[list[list[int]], int, int]:
    try:
        from PIL import Image
    except ImportError as exc:  # pragma: no cover - depends on the environment
        raise SystemExit("Pillow is required to decode frames: pip install -r tools/camera/requirements.txt") from exc
    with Image.open(path) as img:
        width, height = img.size
        data = img.convert("L").tobytes()
    return [list(data[y * width:(y + 1) * width]) for y in range(height)], width, height


def image_files(directory: Path) -> list[Path]:
    return sorted(p for p in directory.iterdir() if p.is_file() and p.suffix.lower() in IMAGE_SUFFIXES)


def analyse(reference_dirs: list[Path], frame_dirs: list[Path], synthetic: bool = False) -> dict:
    ref_files = [p for d in reference_dirs for p in image_files(d)]
    if not ref_files:
        raise SystemExit("no reference frames found")
    ref = fxframes.reference([load_luma(p)[0] for p in ref_files])
    frames = []
    for directory in frame_dirs:
        for path in image_files(directory):
            luma, width, height = load_luma(path)
            data = path.read_bytes()
            m = fxframes.assess(luma, ref)
            name = NODE_NAME.search(path.name)
            frames.append({
                "file": str(path), "trial": directory.name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                "width": width, "height": height, "capture": int(name.group(1)) if name else None,
                "index": int(name.group(2)) if name else None,
                "mean_luma": round(m["mean_luma"], 2), "contrast": round(m["contrast"], 2),
                "sharpness": round(m["sharpness"], 2), "similarity": round(m["similarity"], 4), "flags": m["flags"],
            })
    return {
        "kind": "camera",
        "synthetic": synthetic,
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "reference": {"files": [str(p) for p in ref_files], "mean_luma": round(ref["mean_luma"], 2),
                      "contrast": round(ref["contrast"], 2), "sharpness": round(ref["sharpness"], 2)},
        "thresholds": {"dark_luma": fxframes.DARK_LUMA, "obscured_similarity": fxframes.OBSCURED_SIMILARITY,
                       "obscured_contrast_fraction": fxframes.OBSCURED_CONTRAST,
                       "blurred_detail_fraction": fxframes.BLURRED_SHARPNESS},
        "frames": frames,
    }


def markdown(report: dict) -> str:
    lines = ["# FireX camera-in-the-loop check", ""]
    if report["synthetic"]:
        lines += ["> **SYNTHETIC DATA.** Generated images, not frames from the camera. Nothing here is a measurement.", ""]
    ref = report["reference"]
    lines += [f"Reference: {len(ref['files'])} frames of the clear room, mean luma {ref['mean_luma']}, "
              f"contrast {ref['contrast']}, sharpness {ref['sharpness']}.", "",
              "| Trial | Frame | Bytes | Luma | Contrast | Sharpness | Similarity | Flags |", "|---|---|---|---|---|---|---|---|"]
    for f in report["frames"]:
        index = "-" if f["index"] is None else f"c{f['capture']} f{f['index']}"
        lines.append(f"| {f['trial']} | {index} | {f['bytes']} | {f['mean_luma']:.0f} | {f['contrast']:.1f} | "
                     f"{f['sharpness']:.0f} | {f['similarity']:.2f} | {', '.join(f['flags']) or 'clear'} |")
    lines += ["", "Flags are starting thresholds, not calibrated: look at every flagged frame before drawing a conclusion.", ""]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Measure CRITICAL frames against reference frames of the clear room.",
        epilog="Reference frames come from a CRITICAL fired with `inject flame` on the bench image in the clear, lit "
               "room, exported like any other incident's photos: no remote shutter is involved.")
    ap.add_argument("--reference", required=True, nargs="+", type=Path, help="directories of reference frames")
    ap.add_argument("--frames", required=True, nargs="+", type=Path, help="one directory of frames per trial")
    ap.add_argument("--out", default="reports/camera", help="output path without extension")
    args = ap.parse_args()

    report = analyse(args.reference, args.frames)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    Path(f"{out}.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    Path(f"{out}.md").write_text(markdown(report), encoding="utf-8")
    flagged = sum(1 for f in report["frames"] if f["flags"])
    print(f"{len(report['frames'])} frames checked, {flagged} flagged; wrote {out}.json and {out}.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
