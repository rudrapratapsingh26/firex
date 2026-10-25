"""
Image measures for the M4 camera-in-the-loop checks: does a frame captured at
CRITICAL still show the room, or has smoke or darkness taken it?

Frames are plain lists of rows of 8-bit luma (0-255), so the arithmetic is
standard library only and tested in CI; tools/camera/frame_check.py decodes the
JPEGs with Pillow and calls these.

    mean_luma    average brightness
    contrast     standard deviation of luma
    sharpness    variance of the 4-neighbour Laplacian: edges and texture
    similarity   normalised cross-correlation with the mean reference frame, both
                 reduced to 64 x 48 first

Similarity ignores overall brightness and contrast (a correlation does), so a
dim but intact view still scores high; `dark` says it is dim. Smoke replaces
structure with haze, which lowers similarity and contrast together: `obscured`
needs both. `blurred` compares detail, sharpness divided by contrast squared,
because raw sharpness falls with the square of brightness and would call every
dim frame blurred. The thresholds are starting points for the first bench
session, not calibrated values.
"""
from __future__ import annotations

import math

Image = list[list[float]]

SIZE = (64, 48)
DARK_LUMA = 40.0
OBSCURED_SIMILARITY = 0.5
OBSCURED_CONTRAST = 0.5   # fraction of the reference contrast
BLURRED_SHARPNESS = 0.25  # fraction of the reference sharpness


def _pixels(image: Image) -> list[float]:
    return [float(v) for row in image for v in row]


def mean_luma(image: Image) -> float:
    pixels = _pixels(image)
    return sum(pixels) / len(pixels) if pixels else 0.0


def contrast(image: Image) -> float:
    pixels = _pixels(image)
    if not pixels:
        return 0.0
    m = sum(pixels) / len(pixels)
    return math.sqrt(sum((p - m) ** 2 for p in pixels) / len(pixels))


def sharpness(image: Image) -> float:
    """Variance of 4c - up - down - left - right over the interior pixels."""
    h = len(image)
    w = len(image[0]) if h else 0
    if h < 3 or w < 3:
        return 0.0
    values = []
    for y in range(1, h - 1):
        above, row, below = image[y - 1], image[y], image[y + 1]
        for x in range(1, w - 1):
            values.append(4.0 * row[x] - above[x] - below[x] - row[x - 1] - row[x + 1])
    m = sum(values) / len(values)
    return sum((v - m) ** 2 for v in values) / len(values)


def detail(image: Image) -> float:
    """Sharpness per unit of contrast squared: unchanged by brightness and gain."""
    c = contrast(image)
    return sharpness(image) / (c * c) if c > 1e-9 else 0.0


def resize(image: Image, width: int = SIZE[0], height: int = SIZE[1]) -> Image:
    """Area average when shrinking, nearest pixel when growing."""
    h = len(image)
    w = len(image[0]) if h else 0
    if not w:
        raise ValueError("empty image")
    out = []
    for j in range(height):
        y0 = j * h // height
        y1 = max(y0 + 1, (j + 1) * h // height)
        row = []
        for i in range(width):
            x0 = i * w // width
            x1 = max(x0 + 1, (i + 1) * w // width)
            total = sum(float(image[y][x]) for y in range(y0, y1) for x in range(x0, x1))
            row.append(total / ((y1 - y0) * (x1 - x0)))
        out.append(row)
    return out


def similarity(a: Image, b: Image) -> float:
    """Normalised cross-correlation, -1 to 1, of two images of the same size. 0 if either is flat."""
    pa, pb = _pixels(a), _pixels(b)
    if len(pa) != len(pb) or not pa:
        raise ValueError("images differ in size")
    ma, mb = sum(pa) / len(pa), sum(pb) / len(pb)
    num = sum((x - ma) * (y - mb) for x, y in zip(pa, pb))
    den = math.sqrt(sum((x - ma) ** 2 for x in pa) * sum((y - mb) ** 2 for y in pb))
    return num / den if den > 1e-9 else 0.0


def mean_image(images: list[Image]) -> Image:
    small = [resize(img) for img in images]
    if not small:
        raise ValueError("no reference frames")
    return [[sum(img[y][x] for img in small) / len(small) for x in range(SIZE[0])] for y in range(SIZE[1])]


def reference(images: list[Image]) -> dict:
    """What the clear room looks like: the mean small image and the average measures."""
    return {
        "image": mean_image(images),
        "mean_luma": sum(mean_luma(i) for i in images) / len(images),
        "contrast": sum(contrast(i) for i in images) / len(images),
        "sharpness": sum(sharpness(i) for i in images) / len(images),
        "detail": sum(detail(i) for i in images) / len(images),
    }


def assess(image: Image, ref: dict) -> dict:
    measures = {
        "mean_luma": mean_luma(image),
        "contrast": contrast(image),
        "sharpness": sharpness(image),
        "detail": detail(image),
        "similarity": similarity(resize(image), ref["image"]),
    }
    flags = []
    if measures["mean_luma"] < DARK_LUMA:
        flags.append("dark")
    if measures["similarity"] < OBSCURED_SIMILARITY and measures["contrast"] < OBSCURED_CONTRAST * ref["contrast"]:
        flags.append("obscured")
    if ref["detail"] > 0 and measures["detail"] < BLURRED_SHARPNESS * ref["detail"]:
        flags.append("blurred")
    measures["flags"] = flags
    return measures
