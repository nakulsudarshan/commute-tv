#!/usr/bin/env python3
"""
Generate FontPixel -- a hand-drawn 5x7 bitmap font -- as a C header.

    python3 tools/gen_pixelfont.py            # writes src/oled/font_pixel.h
    python3 tools/gen_pixelfont.py --check    # verify the header is current
    python3 tools/gen_pixelfont.py --show ABC # print glyphs to the terminal

WHY THIS EXISTS, when tools/gen_font.py already bakes fonts.

gen_font.py rasterises a TrueType face. That is right for the 240x240 screen,
where 13-44 px leaves the outline room to be itself. It is wrong at 9 px on a
1-bit panel: stems land between pixels and get dropped or doubled, so some
letters come out spidery and others blobby. Nakul saw exactly that on the real
OLED and asked whether it was the screen -- it was half the screen and half
this.

A glyph drawn ON the pixel grid has no such problem. There is nothing to round.

Two consequences worth having:

  * IT IS REPRODUCIBLE. gen_font.py's output depends on the installed Pillow
    version -- measured 25 Aug 2026, Pillow 12.2 gives 1,233 bytes where the
    committed header has 1,168, silently shifting every glyph. This script has
    no dependencies at all, so the same art always gives the same bytes, and
    --check proves the committed header matches the art.
  * IT IS EDITABLE. The source of truth below is ASCII art. If a letter looks
    wrong on the glass, fix the picture.

Geometry: each glyph is drawn on 5 columns x 8 rows. Rows 0-6 sit above the
baseline, row 7 below it, so capitals are 7 px and descenders drop 1 px. Blank
rows and columns are trimmed at generation time and turned into bearings, so
the blob stays small and narrow glyphs stay narrow.
"""

import argparse
import pathlib
import sys

OUT = pathlib.Path(__file__).resolve().parent.parent / "src" / "oled" / "font_pixel.h"

CELL_W = 5
CELL_H = 8
BASELINE_ROW = 7          # art row 7 is the first row BELOW the baseline
DEFAULT_ADVANCE = 6       # 5 columns of ink + 1 of air

# --- the art -----------------------------------------------------------------
#
# Each entry is (character, 8 rows of 5 columns, advance).
# '#' is ink, '.' is paper. An advance of None means DEFAULT_ADVANCE.
#
# Narrow glyphs are drawn hard against the left edge and given a small advance,
# which is what makes 'i' and 'l' cost 2 px instead of 6. That matters: the
# value column is 90 px, and a stop name is not short.

G = {}

def g(ch, art, advance=None):
    rows = [r for r in art.strip("\n").split("\n")]
    assert len(rows) == CELL_H, f"{ch!r}: {len(rows)} rows, need {CELL_H}"
    for r in rows:
        assert len(r) == CELL_W, f"{ch!r}: row {r!r} is {len(r)} wide, need {CELL_W}"
    G[ch] = (rows, advance if advance is not None else DEFAULT_ADVANCE)

g(" ", """
.....
.....
.....
.....
.....
.....
.....
.....
""", 3)

g("!", """
#....
#....
#....
#....
#....
.....
#....
.....
""", 2)

g('"', """
#.#..
#.#..
.....
.....
.....
.....
.....
.....
""", 4)

g("#", """
.....
.#.#.
#####
.#.#.
#####
.#.#.
.....
.....
""")

g("$", """
..#..
.####
#.#..
.###.
..#.#
####.
..#..
.....
""")

g("%", """
##...
##..#
...#.
..#..
.#...
#..##
...##
.....
""")

g("&", """
.##..
#..#.
#.#..
.#...
#.#.#
#..#.
.##.#
.....
""")

g("'", """
#....
#....
.....
.....
.....
.....
.....
.....
""", 2)

g("(", """
.#...
#....
#....
#....
#....
#....
.#...
.....
""", 3)

g(")", """
#....
.#...
.#...
.#...
.#...
.#...
#....
.....
""", 3)

g("*", """
.....
..#..
#.#.#
.###.
#.#.#
..#..
.....
.....
""")

g("+", """
.....
..#..
..#..
#####
..#..
..#..
.....
.....
""")

g(",", """
.....
.....
.....
.....
.....
.....
.#...
#....
""", 3)

g("-", """
.....
.....
.....
####.
.....
.....
.....
.....
""", 5)

g(".", """
.....
.....
.....
.....
.....
.....
#....
.....
""", 2)

g("/", """
....#
....#
...#.
..#..
.#...
#....
#....
.....
""")

g("0", """
.###.
#...#
#..##
#.#.#
##..#
#...#
.###.
.....
""")

g("1", """
..#..
.##..
..#..
..#..
..#..
..#..
.###.
.....
""")

g("2", """
.###.
#...#
....#
...#.
..#..
.#...
#####
.....
""")

g("3", """
#####
...#.
..#..
...#.
....#
#...#
.###.
.....
""")

g("4", """
...#.
..##.
.#.#.
#..#.
#####
...#.
...#.
.....
""")

g("5", """
#####
#....
####.
....#
....#
#...#
.###.
.....
""")

g("6", """
..##.
.#...
#....
####.
#...#
#...#
.###.
.....
""")

g("7", """
#####
....#
...#.
..#..
.#...
.#...
.#...
.....
""")

g("8", """
.###.
#...#
#...#
.###.
#...#
#...#
.###.
.....
""")

g("9", """
.###.
#...#
#...#
.####
....#
...#.
.##..
.....
""")

g(":", """
.....
.....
#....
.....
.....
#....
.....
.....
""", 2)

g(";", """
.....
.....
.#...
.....
.....
.#...
.#...
#....
""", 3)

g("<", """
.....
...#.
..#..
.#...
..#..
...#.
.....
.....
""")

g("=", """
.....
.....
#####
.....
#####
.....
.....
.....
""")

g(">", """
.....
.#...
..#..
...#.
..#..
.#...
.....
.....
""")

g("?", """
.###.
#...#
....#
...#.
..#..
.....
..#..
.....
""")

g("@", """
.###.
#...#
#.###
#.#.#
#.###
#....
.###.
.....
""")

g("A", """
.###.
#...#
#...#
#####
#...#
#...#
#...#
.....
""")

g("B", """
####.
#...#
#...#
####.
#...#
#...#
####.
.....
""")

g("C", """
.###.
#...#
#....
#....
#....
#...#
.###.
.....
""")

g("D", """
####.
#...#
#...#
#...#
#...#
#...#
####.
.....
""")

g("E", """
#####
#....
#....
####.
#....
#....
#####
.....
""")

g("F", """
#####
#....
#....
####.
#....
#....
#....
.....
""")

g("G", """
.###.
#...#
#....
#.###
#...#
#...#
.###.
.....
""")

g("H", """
#...#
#...#
#...#
#####
#...#
#...#
#...#
.....
""")

g("I", """
.###.
..#..
..#..
..#..
..#..
..#..
.###.
.....
""", 5)

g("J", """
..###
...#.
...#.
...#.
...#.
#..#.
.##..
.....
""")

g("K", """
#...#
#..#.
#.#..
##...
#.#..
#..#.
#...#
.....
""")

g("L", """
#....
#....
#....
#....
#....
#....
#####
.....
""")

g("M", """
#...#
##.##
#.#.#
#.#.#
#...#
#...#
#...#
.....
""")

g("N", """
#...#
##..#
##..#
#.#.#
#..##
#..##
#...#
.....
""")

g("O", """
.###.
#...#
#...#
#...#
#...#
#...#
.###.
.....
""")

g("P", """
####.
#...#
#...#
####.
#....
#....
#....
.....
""")

g("Q", """
.###.
#...#
#...#
#...#
#.#.#
#..#.
.##.#
.....
""")

g("R", """
####.
#...#
#...#
####.
#.#..
#..#.
#...#
.....
""")

g("S", """
.####
#....
#....
.###.
....#
....#
####.
.....
""")

g("T", """
#####
..#..
..#..
..#..
..#..
..#..
..#..
.....
""")

g("U", """
#...#
#...#
#...#
#...#
#...#
#...#
.###.
.....
""")

g("V", """
#...#
#...#
#...#
#...#
#...#
.#.#.
..#..
.....
""")

g("W", """
#...#
#...#
#...#
#.#.#
#.#.#
##.##
#...#
.....
""")

g("X", """
#...#
#...#
.#.#.
..#..
.#.#.
#...#
#...#
.....
""")

g("Y", """
#...#
#...#
.#.#.
..#..
..#..
..#..
..#..
.....
""")

g("Z", """
#####
....#
...#.
..#..
.#...
#....
#####
.....
""")

g("[", """
##...
#....
#....
#....
#....
#....
##...
.....
""", 3)

g("\\", """
#....
#....
.#...
..#..
...#.
....#
....#
.....
""")

g("]", """
##...
.#...
.#...
.#...
.#...
.#...
##...
.....
""", 3)

g("^", """
..#..
.#.#.
#...#
.....
.....
.....
.....
.....
""")

g("_", """
.....
.....
.....
.....
.....
.....
.....
#####
""")

g("`", """
#....
.#...
.....
.....
.....
.....
.....
.....
""", 3)

g("a", """
.....
.....
.###.
....#
.####
#...#
.####
.....
""")

g("b", """
#....
#....
####.
#...#
#...#
#...#
####.
.....
""")

g("c", """
.....
.....
.###.
#....
#....
#....
.###.
.....
""")

g("d", """
....#
....#
.####
#...#
#...#
#...#
.####
.....
""")

g("e", """
.....
.....
.###.
#...#
#####
#....
.###.
.....
""")

g("f", """
..###
.#...
.#...
####.
.#...
.#...
.#...
.....
""")

g("g", """
.....
.....
.####
#...#
#...#
.####
....#
.###.
""")

g("h", """
#....
#....
####.
#...#
#...#
#...#
#...#
.....
""")

g("i", """
#....
.....
#....
#....
#....
#....
#....
.....
""", 2)

g("j", """
...#.
.....
...#.
...#.
...#.
...#.
#..#.
.##..
""")

g("k", """
#....
#....
#..#.
#.#..
##...
#.#..
#..#.
.....
""")

g("l", """
#....
#....
#....
#....
#....
#....
#....
.....
""", 2)

g("m", """
.....
.....
##.#.
#.#.#
#.#.#
#.#.#
#.#.#
.....
""")

g("n", """
.....
.....
####.
#...#
#...#
#...#
#...#
.....
""")

g("o", """
.....
.....
.###.
#...#
#...#
#...#
.###.
.....
""")

g("p", """
.....
.....
####.
#...#
#...#
####.
#....
#....
""")

g("q", """
.....
.....
.####
#...#
#...#
.####
....#
....#
""")

g("r", """
.....
.....
#.##.
##...
#....
#....
#....
.....
""")

g("s", """
.....
.....
.####
#....
.###.
....#
####.
.....
""")

g("t", """
.#...
.#...
####.
.#...
.#...
.#...
..##.
.....
""")

g("u", """
.....
.....
#...#
#...#
#...#
#...#
.####
.....
""")

g("v", """
.....
.....
#...#
#...#
#...#
.#.#.
..#..
.....
""")

g("w", """
.....
.....
#...#
#...#
#.#.#
#.#.#
.#.#.
.....
""")

g("x", """
.....
.....
#...#
.#.#.
..#..
.#.#.
#...#
.....
""")

g("y", """
.....
.....
#...#
#...#
#...#
.####
....#
.###.
""")

g("z", """
.....
.....
#####
...#.
..#..
.#...
#####
.....
""")

g("{", """
..##.
..#..
..#..
.#...
..#..
..#..
..##.
.....
""", 4)

g("|", """
#....
#....
#....
#....
#....
#....
#....
.....
""", 2)

g("}", """
##...
.#...
.#...
..#..
.#...
.#...
##...
.....
""", 4)

g("~", """
.....
.....
.....
.#.##
##.#.
.....
.....
.....
""")

FIRST, LAST = 0x20, 0x7E


def trim(rows):
    """(bits, width, height, xOffset, yOffset) with blank rows/cols removed."""
    filled = [(r, c) for r in range(CELL_H) for c in range(CELL_W) if rows[r][c] == "#"]
    if not filled:
        return b"", 0, 0, 0, 0
    r0 = min(r for r, _ in filled)
    r1 = max(r for r, _ in filled)
    c0 = min(c for _, c in filled)
    c1 = max(c for _, c in filled)
    w, h = c1 - c0 + 1, r1 - r0 + 1
    out = bytearray()
    for r in range(r0, r1 + 1):
        # One byte per row, MSB first -- byte-aligned per row, matching the
        # format render::Glyph already blits.
        for byte_start in range(0, w, 8):
            b = 0
            for bit in range(8):
                c = c0 + byte_start + bit
                if c <= c1 and rows[r][c] == "#":
                    b |= 0x80 >> bit
            out.append(b)
    return bytes(out), w, h, c0, r0 - BASELINE_ROW


def build():
    blob = bytearray()
    table = []
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        if ch not in G:
            raise SystemExit(f"no art for {ch!r} (U+{code:04X})")
        rows, advance = G[ch]
        bits, w, h, xo, yo = trim(rows)
        table.append((len(blob), w, h, xo, yo, advance, ch))
        blob.extend(bits)
    return blob, table


def render_header(blob, table):
    L = []
    L.append("// GENERATED by tools/gen_pixelfont.py -- do not edit by hand.")
    L.append("//")
    L.append("// FontPixel: a 5x7 font DRAWN on the pixel grid, not rasterised onto it.")
    L.append("// Capitals are 7 px, descenders drop 1 px below the baseline, and the widest")
    L.append("// glyph advances 6 px. At 1 bit per pixel there is no anti-aliasing to hide")
    L.append("// behind, so every stem here is exactly one pixel wide by construction --")
    L.append("// which is the whole reason this replaced the 9 px TrueType face on the OLED.")
    L.append("//")
    L.append("// The art is in the generator and is the source of truth. Edit the picture,")
    L.append("// rerun, and `--check` will tell you if this header ever drifts from it.")
    L.append("#pragma once")
    L.append("#include <cstdint>")
    L.append('#include "../render/font.h"')
    L.append("")
    L.append("namespace render {")
    L.append("")
    L.append(f"// ---- FontPixel: hand-drawn 5x7, U+0020..U+007E, {len(blob)} bytes ----")
    L.append("static const uint8_t FontPixel_bits[] PROGMEM_ATTR = {")
    for i in range(0, len(blob), 16):
        chunk = ",".join(f"0x{b:02X}" for b in blob[i:i + 16])
        L.append(f"    {chunk},")
    L.append("};")
    L.append("")
    L.append("static const Glyph FontPixel_glyphs[] PROGMEM_ATTR = {")
    for off, w, h, xo, yo, adv, ch in table:
        shown = ch if ch not in ("\\",) else "backslash"
        L.append(f"    {{{off},{w},{h},{xo},{yo},{adv}}},   // {shown!r}")
    L.append("};")
    L.append("")
    L.append("static const Font FontPixel = {")
    L.append(f"    FontPixel_bits, FontPixel_glyphs, 0x{FIRST:02X}, 0x{LAST:02X}, 9, 7")
    L.append("};")
    L.append("")
    L.append("} // namespace render")
    L.append("")
    return "\n".join(L)


def show(chars):
    for ch in chars:
        if ch not in G:
            print(f"{ch!r}: no art")
            continue
        rows, adv = G[ch]
        print(f"{ch!r}  advance {adv}")
        for r in rows:
            print("   " + r.replace("#", "██").replace(".", "  "))
        print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the header is not what the art produces")
    ap.add_argument("--show", metavar="CHARS", help="print glyphs to the terminal")
    a = ap.parse_args()

    if a.show:
        show(a.show)
        return 0

    blob, table = build()
    text = render_header(blob, table)

    if a.check:
        if not a.out.exists():
            print(f"MISSING: {a.out}", file=sys.stderr)
            return 1
        if a.out.read_text() != text:
            print(f"STALE: {a.out} does not match the art in {__file__}", file=sys.stderr)
            return 1
        print(f"ok: {a.out.name} matches the art ({len(blob)} bytes of glyphs)")
        return 0

    a.out.write_text(text)
    widest = max(t[5] for t in table)
    print(f"wrote {a.out}  --  {len(table)} glyphs, {len(blob)} bytes, widest advance {widest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
