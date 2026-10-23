#!/usr/bin/env python3
"""
Verify the C++ JPEG encoder against an independent decoder.

`make preview` writes each scenario twice: a lossless BMP and the JPEG the
device would actually upload. This decodes both with Pillow -- code that has
never seen ours -- and reports how far apart they are.

    make preview
    python3 tools/verify_jpeg.py

Why this rather than a unit test: a JPEG encoder can produce a file that parses
perfectly and still looks wrong. The only check that means anything is putting
the bytes through a decoder we did not write and comparing pixels.

Also writes preview/out/jpeg_compare.png -- a side-by-side of the worst region
of the worst scenario, magnified, because a PSNR figure says nothing about
whether 12 px amber text is still readable.

    pip install pillow numpy
"""

import pathlib
import struct
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("needs pillow and numpy:  pip install pillow numpy")

# Anchored to this file rather than the shell's working directory, so the tool
# runs from anywhere in the tree.
OUT = pathlib.Path(__file__).resolve().parent.parent / "preview" / "out"

# Thresholds from measurement, not from a textbook: flat UI art encodes far
# better than photographs, so a bar borrowed from photo work would pass
# anything. The useful reference point is that 4:2:0 plateaus at ~30 dB on this
# content no matter the quality setting, and looks visibly damaged there. The
# default (4:4:4, q90) measures ~36 dB. Setting the gate at 35 dB therefore
# fails any regression that drifts back toward subsampled quality, while
# leaving normal encoder variation alone.
MIN_PSNR_DB = 35.0
MIN_TEXT_PSNR_DB = 31.0


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    if mse == 0.0:
        return float("inf")
    return 10.0 * np.log10(255.0 * 255.0 / mse)


def quantise_to_565(img: np.ndarray) -> np.ndarray:
    """The BMP holds RGB565 values already expanded to 8 bits. The JPEG decodes
    to arbitrary 8-bit values. Comparing them directly would charge the encoder
    for the panel's own colour depth, so snap the decoded image to the same
    565 lattice first."""
    # int32 throughout: doing this in uint8 silently overflows at the
    # multiply and turns a good encoder into a terrible-looking one.
    src = img.astype(np.int32)
    out = np.empty_like(src)
    out[..., 0] = (src[..., 0] >> 3) * 255 // 31
    out[..., 1] = (src[..., 1] >> 2) * 255 // 63
    out[..., 2] = (src[..., 2] >> 3) * 255 // 31
    return out.astype(np.uint8)


def jpeg_structure(path: pathlib.Path) -> dict:
    """Walk the marker segments. Confirms the file is baseline, three-component
    and self-contained -- properties the SmallTV's decoder will assume."""
    data = path.read_bytes()
    if data[:2] != b"\xff\xd8":
        raise ValueError("no SOI")
    if data[-2:] != b"\xff\xd9":
        raise ValueError("no EOI")

    info = {"markers": [], "sof": None, "dqt": 0, "dht": 0}
    i = 2
    while i < len(data) - 1:
        if data[i] != 0xFF:
            raise ValueError(f"expected marker at {i}, got {data[i]:#04x}")
        marker = data[i + 1]
        info["markers"].append(marker)
        if marker == 0xD9:
            break
        seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
        body = data[i + 4:i + 2 + seg_len]
        if marker == 0xC0:
            prec, h, w, ncomp = body[0], *struct.unpack(">HH", body[1:5]), body[5]
            info["sof"] = {"precision": prec, "height": h, "width": w,
                           "components": ncomp,
                           "sampling": [body[6 + 3 * c + 1] for c in range(ncomp)]}
        elif marker == 0xDB:
            info["dqt"] += 1
        elif marker == 0xC4:
            info["dht"] += 1
        elif marker == 0xDA:
            break  # entropy-coded data follows; scanning it is the decoder's job
        i += 2 + seg_len
    return info


def worst_tile(a: np.ndarray, b: np.ndarray, size: int = 64):
    """Find the size x size tile with the lowest PSNR -- the place the encoder
    is doing its worst work, which is what we actually want to look at."""
    h, w = a.shape[:2]
    worst, best_xy = float("inf"), (0, 0)
    for y in range(0, h - size + 1, 16):
        for x in range(0, w - size + 1, 16):
            p = psnr(a[y:y + size, x:x + size], b[y:y + size, x:x + size])
            if p < worst:
                worst, best_xy = p, (x, y)
    return worst, best_xy


def main() -> int:
    pairs = sorted(OUT.glob("*.bmp"))
    if not pairs:
        sys.exit(f"no previews in {OUT} -- run `make preview` first")

    print(f"{'scenario':<20} {'bytes':>7} {'PSNR':>8} {'worst 64px':>11}  structure")
    print("-" * 72)

    failures = []
    records = []
    for bmp in pairs:
        jpg = bmp.with_suffix(".jpg")
        if not jpg.exists():
            failures.append(f"{bmp.stem}: no JPEG was produced")
            continue

        ref = np.asarray(Image.open(bmp).convert("RGB"))
        dec = np.asarray(Image.open(jpg).convert("RGB"))

        if ref.shape != dec.shape:
            failures.append(f"{bmp.stem}: size {dec.shape} != {ref.shape}")
            continue

        snapped = quantise_to_565(dec)
        whole = psnr(ref, snapped)
        tile_db, tile_xy = worst_tile(ref, snapped)

        try:
            info = jpeg_structure(jpg)
            sof = info["sof"]
            struct_ok = (sof and sof["precision"] == 8 and sof["components"] == 3
                         and 0xC0 in info["markers"] and info["dqt"] >= 1
                         and info["dht"] >= 2)
            struct_txt = (f"baseline {sof['width']}x{sof['height']} "
                          f"{sof['components']}ch samp={[hex(s) for s in sof['sampling']]}")
            if not struct_ok:
                failures.append(f"{bmp.stem}: structure looks wrong -- {info}")
        except ValueError as e:
            struct_txt = f"BAD: {e}"
            failures.append(f"{bmp.stem}: {e}")

        print(f"{bmp.stem:<20} {jpg.stat().st_size:>7} {whole:>7.1f}dB "
              f"{tile_db:>9.1f}dB  {struct_txt}")

        if whole < MIN_PSNR_DB:
            failures.append(f"{bmp.stem}: whole-image PSNR {whole:.1f} dB "
                            f"below {MIN_PSNR_DB} dB")
        if tile_db < MIN_TEXT_PSNR_DB:
            failures.append(f"{bmp.stem}: worst tile {tile_db:.1f} dB at {tile_xy} "
                            f"below {MIN_TEXT_PSNR_DB} dB")

        records.append((tile_db, bmp.stem, ref, dec, tile_xy))

    # --- eyeball the worst case, magnified ---------------------------------
    if records:
        records.sort(key=lambda r: r[0])
        _, name, ref, dec, (x, y) = records[0]
        size, zoom = 64, 5
        crop_ref = Image.fromarray(ref[y:y + size, x:x + size]).resize(
            (size * zoom, size * zoom), Image.NEAREST)
        crop_dec = Image.fromarray(dec[y:y + size, x:x + size]).resize(
            (size * zoom, size * zoom), Image.NEAREST)
        sheet = Image.new("RGB", (size * zoom * 2 + 12, size * zoom), (40, 40, 40))
        sheet.paste(crop_ref, (0, 0))
        sheet.paste(crop_dec, (size * zoom + 12, 0))
        sheet.save(OUT / "jpeg_compare.png")
        print(f"\nworst region: {name} at ({x},{y})")
        print(f"  canvas (left) vs decoded JPEG (right) -> {OUT / 'jpeg_compare.png'}")

    print()
    if failures:
        print(f"{len(failures)} PROBLEM(S):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"All {len(records)} scenarios pass: structure valid, "
          f"PSNR >= {MIN_PSNR_DB} dB, worst tile >= {MIN_TEXT_PSNR_DB} dB.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
