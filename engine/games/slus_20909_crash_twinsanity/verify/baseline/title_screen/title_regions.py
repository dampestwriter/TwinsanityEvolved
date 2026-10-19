#!/usr/bin/env python3
"""title_regions.py -- per-region statistics of the title screen for the three defect regions ORB / LOGO /
OCEAN, reference first (the PCSX2 dump's 640x480 screenshot, resized to 512x448), then any number of frames.
Each frame's orientation is auto-detected (the sky's top rows are warmer than the ocean's: R > B → top-down;
else the image is stored bottom-up and is flipped), so OLD's top-down PPMs and NEW's bottom-up `--dump-frames`
/ `gs_replay --dump-ppm` PPMs measure alike.  Prints per region: mean RGB, the logo's yellow-pixel mean
(R>150, R>G>B, B<120) + count, the bright share (max channel >= 230), the ocean's light-pixel count (R>120,
G>150), and an FNV-1a hash of the region's bytes.  `--json out` writes the numbers.
Usage: python3 -u verify/baseline/title_screen/title_regions.py <ref> <img>... [--json out.json] [--noauto]"""
import json
import sys
from PIL import Image

REGIONS = {  # 512x448 frame coordinates, top-down
    "ORB": (196, 150, 316, 290),
    "LOGO": (104, 110, 416, 236),
    "OCEAN": (0, 318, 512, 388),
}


def fnv1a(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def orient(im):
    """True when the stored image is bottom-up (first rows are the ocean: blue over red)."""
    w, h = im.size
    top = im.crop((0, 8, w, 32)).getdata()
    n = len(top)
    r = sum(p[0] for p in top) / n
    b = sum(p[2] for p in top) / n
    return b > r


def load(path, is_ref, auto):
    im = Image.open(path).convert("RGB")
    flipped = False
    if is_ref:
        if im.size != (512, 448):
            im = im.resize((512, 448), Image.BILINEAR)
    elif auto and orient(im):
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
        flipped = True
    return im, flipped


def stats(im, box):
    reg = im.crop(box)
    px = list(reg.getdata())
    n = len(px)
    sr = sg = sb = bright = 0
    ys = [0, 0, 0, 0]
    light = 0
    for r, g, b in px:
        sr += r; sg += g; sb += b
        if max(r, g, b) >= 230: bright += 1
        if r > 150 and r > g > b and b < 120:
            ys[0] += r; ys[1] += g; ys[2] += b; ys[3] += 1
        if r > 120 and g > 150: light += 1
    yn = ys[3]
    return dict(mean=(sr / n, sg / n, sb / n), bright=bright / n,
                yellow=((ys[0] / yn, ys[1] / yn, ys[2] / yn) if yn else (0, 0, 0)), yellow_n=yn,
                light=light, hash="%016x" % fnv1a(reg.tobytes()))


def main(argv):
    auto = "--noauto" not in argv
    jout = argv[argv.index("--json") + 1] if "--json" in argv else None
    files = [a for a in argv[1:] if not a.startswith("--") and a != jout]
    out = {}
    for i, f in enumerate(files):
        im, flipped = load(f, i == 0, auto)
        lab = "/".join(f.split("/")[-2:])
        out[lab] = {}
        print("%s%s" % (lab, " (flipped: stored bottom-up)" if flipped else ""))
        for name, box in REGIONS.items():
            s = stats(im, box)
            out[lab][name] = s
            print("  %-5s mean=(%5.1f,%5.1f,%5.1f) bright=%.3f yellow=(%3.0f,%3.0f,%3.0f) n=%-5d light=%-5d hash=%s" % (
                name, s["mean"][0], s["mean"][1], s["mean"][2], s["bright"], s["yellow"][0], s["yellow"][1],
                s["yellow"][2], s["yellow_n"], s["light"], s["hash"]))
    if jout:
        json.dump(out, open(jout, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
