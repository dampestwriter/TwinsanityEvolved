#!/usr/bin/env python3
"""pdesc.py -- the perceptual frame descriptor, the bit-for-bit mirror of te::harness::Describe
(platform/nativeee/host/te_harness.cpp): what the port writes per presented frame into its harness leg's frames.pdesc
and what tools/oracle/diff.py computes for the recomp oracle's dumped frames (its --dump-frames PPMs) so the two legs
compare perceptually when their frame hashes differ (they do: different disc region, renderer path, letterbox).

One row (after the frame key): w,h,lum_sky,lum_mid,lum_ground,mean_r,mean_g,mean_b,clear_share,dhash
  lum_*        mean luminance (299 R + 587 G + 114 B) / 1000 of the arena-judge bands: x 5..95 % of the width,
               sky y 0..25 %, mid 30..55 %, ground 60..95 % of the height (integer percent bounds, like the C++)
  mean_*       mean R, G, B over the whole frame
  clear_share  the share of the most common 12-bit (4 bits per channel) colour -- a frame that is mostly one colour
               has no world (the judges' clear-colour share)
  dhash        a 9x8 difference hash of the integer mean luminance per cell (bit cy*8+cx set when cell[cx] < cell[cx+1]):
               Hamming distance is the structural distance between two frames

Usage: pdesc.py FRAME.ppm [...]        one "key,row" line per file (key = the digits after the leading f/frame_)
       (import: describe(w, h, rgb_bytes, channels) -> str; read_ppm(path) -> (w, h, bytes))
Change the C++ and this file together; platform/nativeee/tests/test_harness.cpp pins their equality.
"""
import os
import re
import sys

BANDS = ((0, 25), (30, 55), (60, 95))


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    tokens = []
    pos = 0
    while len(tokens) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    pos += 1
    if tokens[0] != b"P6":
        raise ValueError(f"{path}: not a binary PPM")
    w, h = int(tokens[1]), int(tokens[2])
    px = data[pos:pos + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError(f"{path}: truncated ({len(px)} of {w * h * 3} bytes)")
    return w, h, px


def describe(w, h, px, channels=3):
    """The descriptor row (without the key) of a top-down RGB8/RGBA8 image, exactly as te::harness::Describe prints it."""
    if w <= 0 or h <= 0 or channels not in (3, 4):
        return ""
    x0, x1 = w * 5 // 100, w * 95 // 100
    rs = gs = bs = 0
    bins = [0] * 4096
    cell_sum = [[0] * 9 for _ in range(8)]
    cell_n = [[0] * 9 for _ in range(8)]
    stride = w * channels
    # whole-frame passes, one row at a time (pure Python: ~0.3 s per 512x448 frame)
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        cy = y * 8 // h
        rch = row[0::channels]
        gch = row[1::channels]
        bch = row[2::channels]
        rs += sum(rch)
        gs += sum(gch)
        bs += sum(bch)
        cs = cell_sum[cy]
        cn = cell_n[cy]
        for x in range(w):
            r, g, b = rch[x], gch[x], bch[x]
            bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)] += 1
            cx = x * 9 // w
            cs[cx] += 299 * r + 587 * g + 114 * b
            cn[cx] += 1
    lum = []
    for y0p, y1p in BANDS:
        y0, y1 = h * y0p // 100, h * y1p // 100
        total = 0
        n = 0
        for y in range(y0, y1):
            row = px[y * stride:(y + 1) * stride]
            seg_r = row[x0 * channels:x1 * channels:channels]
            seg_g = row[x0 * channels + 1:x1 * channels:channels]
            seg_b = row[x0 * channels + 2:x1 * channels:channels]
            total += 299 * sum(seg_r) + 587 * sum(seg_g) + 114 * sum(seg_b)
            n += len(seg_r)
        lum.append(float(total) / (1000.0 * float(n)) if n else 0.0)
    top = max(bins)
    n = float(w) * float(h)
    dhash = 0
    for cy in range(8):
        for cx in range(8):
            a = cell_sum[cy][cx] // cell_n[cy][cx] if cell_n[cy][cx] else 0
            b = cell_sum[cy][cx + 1] // cell_n[cy][cx + 1] if cell_n[cy][cx + 1] else 0
            if a < b:
                dhash |= 1 << (cy * 8 + cx)
    return "%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%016x" % (w, h, lum[0], lum[1], lum[2], rs / n, gs / n, bs / n, top / n, dhash)


def frame_key(path):
    m = re.match(r"^(?:f|frame_)0*(\d+)", os.path.basename(path))
    return int(m.group(1)) if m else -1


def parse_row(line):
    """A frames.pdesc line -> dict, or None for comments/blank/no-scanout rows."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    f = line.split(",")
    if len(f) != 11:
        return None
    row = {"key": int(f[0]), "w": int(f[1]), "h": int(f[2]), "sky": float(f[3]), "mid": float(f[4]), "ground": float(f[5]),
           "r": float(f[6]), "g": float(f[7]), "b": float(f[8]), "clear_share": float(f[9]), "dhash": int(f[10], 16) if f[10] else 0}
    return row


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        w, h, px = read_ppm(path)
        print(f"{frame_key(path)},{describe(w, h, px, 3)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
