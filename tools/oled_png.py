#!/usr/bin/env python3
"""
Turn the .pgm files `make oled` writes into a PNG that looks like the panel.

    make oled && python3 tools/oled_png.py

A 128x64 PGM opened in an image viewer is a postage stamp, and white-on-white
tells you nothing about how a monochrome OLED actually reads. This scales the
frames up, paints them in the module's own blue on black, and stacks them with
captions -- close enough to judge legibility against a photo of the real thing.

Optional: needs Pillow. Nothing in the build depends on it.
"""

import pathlib
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs Pillow:  pip install pillow --break-system-packages")

OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "preview" / "out"
# `make oled` already writes its PGMs at 4x, and the off pixels in them are a
# dark grey rather than a true zero -- so threshold, never truth-test.
SCALE = 2
ON_THRESHOLD = 128
MARGIN = 10
GAP = 26
ON = (120, 205, 255)      # the blue these SSD1306 modules actually emit
OFF = (6, 8, 14)
CAPTION = (150, 160, 175)

PANELS = [
    ("oled_1_armed.pgm", "holding the SmallTV -- everything working"),
    ("oled_2_idle.pgm", "nothing scheduled, nothing fetched yet"),
    ("oled_3_broken.pgm", "the panel doing its actual job"),
    ("oled_4_boot.pgm", "the first second"),
]


def read_pgm(path):
    data = path.read_bytes()
    # P5 header: magic, width, height, maxval -- whitespace separated, and
    # comment lines beginning '#' are legal anywhere in it.
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, _ = fields
    return w, h, data[i + 1 : i + 1 + w * h]


def main():
    found = [(OUT_DIR / n, c) for n, c in PANELS if (OUT_DIR / n).exists()]
    if not found:
        sys.exit(f"no .pgm files in {OUT_DIR} -- run `make oled` first")

    w, h, _ = read_pgm(found[0][0])
    pw, ph = w * SCALE, h * SCALE
    sheet = Image.new("RGB",
                      (pw + 2 * MARGIN, MARGIN + len(found) * (ph + GAP)),
                      (18, 18, 22))
    d = ImageDraw.Draw(sheet)

    for idx, (path, caption) in enumerate(found):
        pw_, ph_, px = read_pgm(path)
        frame = Image.new("RGB", (pw_, ph_))
        frame.putdata([ON if b >= ON_THRESHOLD else OFF for b in px])
        frame = frame.resize((pw_ * SCALE, ph_ * SCALE), Image.NEAREST)
        y = MARGIN + idx * (ph + GAP)
        sheet.paste(frame, (MARGIN, y))
        d.text((MARGIN, y + ph + 6), caption, fill=CAPTION)

    out = OUT_DIR / "oled_panels.png"
    sheet.save(out)
    print(f"wrote {out}  ({sheet.width}x{sheet.height}, {len(found)} panels at {SCALE}x)")


if __name__ == "__main__":
    main()
