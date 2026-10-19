#!/usr/bin/env python3
"""display_judge.py -- does a DISPLAY setting change show in the PRESENTED frame?

Two deterministic headless runs of the title to the main menu (fixtures/judges/display_pal.json; frames dumped every 100
presents): A with the default shape (`auto` = the frame's own pixels when headless: the 512x512 PAL frame fills the capture)
and B with `--aspect 16:9` (the frame letterboxed to 16:9: a 512x288 picture at y 112 with black bars above and below). GREEN
iff B's bars (the rows above and below the picture) are black (mean luminance <= `bar_max_luminance`) while A's same rows are
NOT (the title's sky and sea: mean >= `picture_min_luminance`), and B's picture rows still hold the scene (mean >=
`picture_min_luminance`). A runner without `--aspect` (a play binary without display settings refuses the option) is RED: the setting does
not exist, so no change can show.

RED on bin/twinsanityevolved.32e1174, GREEN on the display-settings build.

Usage: display_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "display_pal.json")


def run(binary, iso, out, fx, extra, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    dump, mc = os.path.join(d, "dump"), os.path.join(d, "mc")
    os.makedirs(dump)
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    print("display_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("display_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, memoryview(data)[pos:pos + 3 * w * h]


def band_luminance(path, y0f, y1f):
    w, h, px = read_ppm(path)
    y0, y1 = int(round(y0f * h)), int(round(y1f * h))
    total, n = 0.0, 0
    for y in range(y0, y1):
        row = px[y * w * 3:(y + 1) * w * 3]
        for i in range(0, len(row), 3):
            total += 0.299 * row[i] + 0.587 * row[i + 1] + 0.114 * row[i + 2]
            n += 1
    return total / n if n else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/display_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or os.path.join(ROOT, "build", "display_judge")
    os.makedirs(out, exist_ok=True)
    present = fx["present"]
    bands = fx["bars"]          # [[y0, y1], ...] as fractions of the frame height: the 16:9 letterbox bars
    picture = fx["picture"]     # [y0, y1]: inside the 16:9 picture

    da, rca = run(a.bin, a.iso, out, fx, [], "A_auto")
    if rca != 0:
        print("display_judge: run A exited %d" % rca, file=sys.stderr)
        return 2
    db, rcb = run(a.bin, a.iso, out, fx, ["--aspect", fx["aspect"]], "B_aspect")
    fails = []
    result = {"present": present, "aspect": fx["aspect"]}
    if rcb != 0:
        fails.append("the runner refused --aspect %s (exit %d): no display setting exists -- nothing can show" % (fx["aspect"], rcb))
    else:
        fa = os.path.join(da, "dump", "frame_%06d.ppm" % present)
        fb = os.path.join(db, "dump", "frame_%06d.ppm" % present)
        if not os.path.exists(fa) or not os.path.exists(fb):
            print("display_judge: present %d not dumped" % present, file=sys.stderr)
            return 2
        for i, (y0, y1) in enumerate(bands):
            la, lb = band_luminance(fa, y0, y1), band_luminance(fb, y0, y1)
            result["bar%d" % i] = {"A": round(la, 1), "B": round(lb, 1)}
            if lb > fx["bar_max_luminance"]:
                fails.append("B's bar %d (rows %.2f..%.2f) is not black: %.1f > %.1f" % (i, y0, y1, lb, fx["bar_max_luminance"]))
            if la < fx["picture_min_luminance"]:
                fails.append("A's rows %.2f..%.2f show no picture: %.1f < %.1f (the default should fill the capture)" % (y0, y1, la, fx["picture_min_luminance"]))
        lp = band_luminance(fb, picture[0], picture[1])
        result["picture"] = {"B": round(lp, 1)}
        if lp < fx["picture_min_luminance"]:
            fails.append("B's picture rows %.2f..%.2f are dark: %.1f < %.1f" % (picture[0], picture[1], lp, fx["picture_min_luminance"]))
    result["verdict"] = "GREEN" if not fails else "RED"
    result["why"] = fails
    print(json.dumps(result))
    print("display_judge: %s" % result["verdict"])
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
