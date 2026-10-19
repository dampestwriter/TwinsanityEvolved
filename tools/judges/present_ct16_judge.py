#!/usr/bin/env python3
"""present_ct16_judge.py -- does the RETAIL PRESENT option show the frame as the GS's dithered 16-bit copy?

Crash Twinsanity drew its frame in 24-bit colour and copied it, with the GS's dithering (DTHE + Sony's DIMX), into the 16-bit
buffer the CRTC displayed (TR renderer/frame.cpp). The port's present is 32-bit; `--present-ct16` / TE_GFX_PRESENT_CT16=1 asks for
the retail look. fixtures/judges/title_present_ct16_pal.json runs the title (deterministic, headless, --skip-movies) with
TE_GFX_PRESENT_CT16=1 and dumps present `present`; on that frame:

  every channel of (almost) every pixel sits on one of PSMCT16's 32 levels      share >= min_on_level_share
  each channel shows at most 32 distinct values                                   <= max_levels
  and the frame is a frame, not flat                                              >= min_levels distinct values per channel

Thresholds FROZEN: RED on bin/twinsanityevolved.32e1174 (the option unknown: 256 levels a channel, 34 % of the pixels happen to
sit on a level), GREEN on the CT16-present branch (32 / 32 / 32 levels, share 1.000).

Usage: present_ct16_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       present_ct16_judge.py --dir DUMP [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / no frame.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import skin_judge  # noqa: E402  (read_ppm)

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "title_present_ct16_pal.json")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    dump = os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--stop-at", str(fx["stop_at"]), "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 300))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_AT"] = str(fx["present"])
    env["TE_GFX_PRESENT_CT16"] = "1"
    print("present_ct16_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("present_ct16_judge: exit", rc, "->", log, flush=True)
    return dump, rc


def on_level(v):
    level = (v * 31 + 127) // 255
    back = (level * 255 + 15) // 31
    return abs(back - v) <= 1


def judge(dump, fx):
    path = os.path.join(dump, "frame_%06d.ppm" % fx["present"])
    if not os.path.exists(path):
        print("present_ct16_judge: RED -- frame %d not dumped" % fx["present"])
        return 1
    w, h, px = skin_judge.read_ppm(path)
    if [w, h] != list(fx["frame"]):
        print("present_ct16_judge: RED -- frame is %dx%d, not %dx%d" % (w, h, fx["frame"][0], fx["frame"][1]))
        return 1
    levels = [set(), set(), set()]
    on = 0
    n = w * h
    for i in range(0, n * 3, 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        levels[0].add(r)
        levels[1].add(g)
        levels[2].add(b)
        if on_level(r) and on_level(g) and on_level(b):
            on += 1
    share = on / n
    counts = [len(s) for s in levels]
    fails = []
    if share < fx["min_on_level_share"]:
        fails.append("only %.3f of the pixels sit on PSMCT16's levels (min %.3f): the frame is not the 16-bit copy" % (share, fx["min_on_level_share"]))
    if max(counts) > fx["max_levels"]:
        fails.append("a channel shows %d distinct values (max %d)" % (max(counts), fx["max_levels"]))
    if min(counts) < fx["min_levels"]:
        fails.append("a channel shows only %d distinct values (min %d): a flat frame" % (min(counts), fx["min_levels"]))
    m = {"present": fx["present"], "levels_rgb": counts, "on_level_share": round(share, 4), "verdict": "RED" if fails else "GREEN"}
    if fails:
        m["why"] = "; ".join(fails)
    print(json.dumps(m))
    print("present_ct16_judge: %s" % m["verdict"])
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/present_ct16_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        return judge(a.dir, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir)")
    out = a.out or os.path.join(ROOT, "build", "present_ct16_judge")
    dump, rc = run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("present_ct16_judge: RED -- the run exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, fx)


if __name__ == "__main__":
    sys.exit(main())
