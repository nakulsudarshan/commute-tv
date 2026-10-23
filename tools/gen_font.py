#!/usr/bin/env python3
"""
Generate 1-bit bitmap fonts as C headers.

The ESP32 has no font rasteriser, so glyphs are baked at build time. This is a
developer tool -- the generated header is committed, and you only need to rerun
it if you want different sizes or typefaces.

    python3 tools/gen_font.py --list
    python3 tools/gen_font.py --only FontTiny --out src/oled/font_tiny.h

⚠️ REGENERATING font_data.h CHANGES THE SCREEN.

Different Pillow versions rasterise the same TTF at the same pixel size
differently. Measured 25 Aug 2026: Pillow 12.2 produces 1,233 bytes of glyph
data for FontSmall where the committed header has 1,168 -- so a plain rerun
silently shifts every glyph on the 240x240 display, invalidates the twelve
verified preview images, and breaks the byte-identical band-rendering tests.

So the default target refuses to be overwritten without --force. Adding a NEW
font to its own header is safe and needs no flag; that is what --only/--out is
for.

Format is Adafruit-GFX-like: packed 1bpp rows, byte-aligned per row, with a
per-glyph offset/size/bearing table. Chosen because it is compact and trivial
to blit without any decoding.
"""

import argparse
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = pathlib.Path(__file__).resolve().parent.parent / "src" / "render" / "font_data.h"

DEJAVU = "/usr/share/fonts/truetype/dejavu/"

# (C identifier, ttf path, pixel size, characters to include)
ASCII = "".join(chr(c) for c in range(32, 127))
DIGITS = "0123456789: +-"

FONTS = [
    ("FontSmall",  DEJAVU + "DejaVuSansCondensed-Bold.ttf", 13, ASCII),
    ("FontMedium", DEJAVU + "DejaVuSans-Bold.ttf",          18, ASCII),
    ("FontLarge",  DEJAVU + "DejaVuSans-Bold.ttf",          44, DIGITS),
    # For the 128x64 OLED health panel. At 9 px this gives eight lines of about
    # twenty characters, which is what it takes to show state rather than a
    # headline. Lives in its own header so generating it cannot disturb the
    # three fonts the 240x240 screen depends on.
    ("FontTiny",   DEJAVU + "DejaVuSansCondensed.ttf",       9, ASCII),
]


def render_glyph(font, ch):
    """Return (width, height, xoff, yoff, advance, rows_of_bits).

    yoff is relative to the BASELINE, negative meaning above it.

    PIL's getbbox() measures from the ascender line, not the baseline, so it
    has to be rebased by subtracting the ascent. Getting this wrong offsets
    every glyph downward by the ascent -- which for a 44px font pushes the
    digits entirely out of their band, and for small text just looks subtly
    misaligned. Both were observed before this was fixed.
    """
    mask = font.getmask(ch, mode="1")
    w, h = mask.size
    bbox = font.getbbox(ch)
    ascent, _ = font.getmetrics()
    advance = int(round(font.getlength(ch)))

    if w == 0 or h == 0:
        return 0, 0, 0, 0, advance, []

    img = Image.frombytes("L", (w, h), bytes(mask))
    px = img.load()
    rows = []
    for y in range(h):
        row = 0
        for x in range(w):
            if px[x, y] > 127:
                row |= 1 << (w - 1 - x)
        rows.append(row)
    xoff = bbox[0] if bbox else 0
    yoff = (bbox[1] - ascent) if bbox else 0      # rebase onto the baseline
    return w, h, xoff, yoff, advance, rows


def pack(rows, width):
    """Pack bit rows into bytes, each row starting on a byte boundary."""
    stride = (width + 7) // 8
    out = bytearray()
    for row in rows:
        for b in range(stride):
            shift = width - (b + 1) * 8
            byte = (row >> shift) & 0xFF if shift >= 0 else (row << -shift) & 0xFF
            out.append(byte)
    return out


def emit(name, path, size, chars, lines):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()

    blob = bytearray()
    table = []
    lo, hi = ord(min(chars)), ord(max(chars))
    included = set(chars)

    for code in range(lo, hi + 1):
        ch = chr(code)
        if ch not in included:
            table.append((0, 0, 0, 0, 0, 0))
            continue
        w, h, xo, yo, adv, rows = render_glyph(font, ch)
        off = len(blob)
        blob += pack(rows, w)
        table.append((off, w, h, xo, yo, adv))

    lines.append(f"// ---- {name}: {pathlib.Path(path).name} @ {size}px, "
                 f"U+{lo:04X}..U+{hi:04X}, {len(blob)} bytes ----")
    lines.append(f"static const uint8_t {name}_bits[] PROGMEM_ATTR = {{")
    for i in range(0, len(blob), 16):
        chunk = ",".join(f"0x{b:02X}" for b in blob[i:i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")

    lines.append(f"static const Glyph {name}_glyphs[] PROGMEM_ATTR = {{")
    for off, w, h, xo, yo, adv in table:
        lines.append(f"    {{{off},{w},{h},{xo},{yo},{adv}}},")
    lines.append("};")

    lines.append(f"static const Font {name} = {{")
    lines.append(f"    {name}_bits, {name}_glyphs, 0x{lo:02X}, 0x{hi:02X}, "
                 f"{ascent + descent}, {ascent}")
    lines.append("};")
    lines.append("")
    return len(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", action="append", default=[],
                    help="generate just this font; repeatable")
    ap.add_argument("--out", default=None, help="write here instead of font_data.h")
    ap.add_argument("--force", action="store_true",
                    help="allow overwriting font_data.h -- see the warning above")
    ap.add_argument("--list", action="store_true", help="show the known fonts and exit")
    args = ap.parse_args()

    if args.list:
        for name, path, size, chars in FONTS:
            print(f"  {name:<12} {size:>3} px  {len(chars):>3} glyphs  "
                  f"{pathlib.Path(path).name}")
        return 0

    wanted = FONTS if not args.only else [f for f in FONTS if f[0] in args.only]
    if not wanted:
        print(f"no such font: {', '.join(args.only)}", file=sys.stderr)
        return 2

    out = pathlib.Path(args.out).resolve() if args.out else OUT
    if out == OUT and not args.force:
        print("Refusing to overwrite font_data.h.\n\n"
              "Pillow rasterises differently between versions, so regenerating it\n"
              "would shift every glyph on the 240x240 display and invalidate the\n"
              "verified previews. Pass --force if that is genuinely what you want,\n"
              "and re-run `make preview` and tools/verify_jpeg.py afterwards.\n\n"
              "To add a new font instead:\n"
              "  python3 tools/gen_font.py --only FontTiny --out src/oled/font_tiny.h",
              file=sys.stderr)
        return 1

    lines = [
        "// GENERATED by tools/gen_font.py -- do not edit by hand.",
        "//",
        "// 1bpp glyph bitmaps, byte-aligned per row. Baked at build time because",
        "// the ESP32 has no font rasteriser.",
        "//",
        "// Regenerating this is NOT reproducible across Pillow versions -- see the",
        "// warning in tools/gen_font.py before you try.",
        "#pragma once",
        "#include <cstdint>",
        "#include \"" + ("../render/font.h" if out != OUT else "font.h") + "\"",
        "",
        "namespace render {",
        "",
    ]
    total = 0
    for name, path, size, chars in wanted:
        total += emit(name, path, size, chars, lines)
    lines.append("} // namespace render")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out}  ({total} bytes of glyph data, {out.stat().st_size:,} bytes of source)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
