#!/usr/bin/env python3
"""shadow_judge.py -- are the shadow volumes composited as the GS did (a footprint under Crash, nothing stamped over
him, no streak up the frame)? The engine's RootShadowLevelEntry criteria on the port's PAL hub frame
(fixtures/judges/hub_shadow_pal.json; the hub pad log, deterministic headless, frames dumped every 100 presents). On
each judged Playing present:

  footprint   foot_darker_min <= ring - feet <= foot_darker_max   (the sand between Crash's feet darkened by the shadow's
                                                                   quarter, not black)
  fur         the body box reads orange (red >= fur_red_min, green <= 0.6 red, blue <= 0.25 red): Crash drawn OVER the
              volume, not under its stamp
  white run   no column below streak_from_row carries >= white_run_max pixels with min(r,g,b) >= white_min_channel
  dark run    no column below streak_from_row carries >= dark_run_max pixels with max(r,g,b) < dark_max_channel (the
              stamped volume over his body)
  band        the column band above Crash is at most streak_max_darker below the bands beside it (the volume's wall rising
              up the frame)

A present whose GameController state is not `require_state` is skipped; fewer than 3 judged presents is RED. Thresholds
are FROZEN in the fixture (red on bin/twinsanityevolved.1952747, green on the stencil-counted volumes).

Usage: shadow_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       shadow_judge.py --dir DUMP --log run.log [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / no frames.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import skin_judge  # noqa: E402  (the run, the frame-state map and the PPM reader are shared)

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_shadow_pal.json")


def lum(px, w, box):
    x0, y0, x1, y1 = box
    total = 0.0
    n = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            total += 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]
            n += 1
    return total / n if n else 0.0


def mean_rgb(px, w, box):
    x0, y0, x1, y1 = box
    r = g = b = 0
    n = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            r += px[i]
            g += px[i + 1]
            b += px[i + 2]
            n += 1
    return (r / n, g / n, b / n) if n else (0.0, 0.0, 0.0)


def longest_run(px, w, h, from_row, pred):
    best, best_x = 0, -1
    for x in range(w):
        run = 0
        for y in range(from_row, h):
            i = (y * w + x) * 3
            if pred(px[i], px[i + 1], px[i + 2]):
                run += 1
                if run > best:
                    best, best_x = run, x
            else:
                run = 0
    return best, best_x


def measure(path, fx):
    w, h, px = skin_judge.read_ppm(path)
    if [w, h] != fx["frame"]:
        return None
    feet = lum(px, w, fx["foot_box"])
    ring = sum(lum(px, w, b) for b in fx["ring_boxes"]) / len(fx["ring_boxes"])
    body = mean_rgb(px, w, fx["body_box"])
    white, white_x = longest_run(px, w, h, fx["streak_from_row"], lambda r, g, b: min(r, g, b) >= fx["white_min_channel"])
    dark, dark_x = longest_run(px, w, h, fx["streak_from_row"], lambda r, g, b: max(r, g, b) < fx["dark_max_channel"])
    centre = lum(px, w, fx["streak_centre_box"])
    sides = sum(lum(px, w, b) for b in fx["streak_side_boxes"]) / len(fx["streak_side_boxes"])
    return {"ring_minus_feet": round(ring - feet, 1), "body": [round(v, 1) for v in body], "white_run": white, "white_column": white_x,
            "dark_run": dark, "dark_column": dark_x, "band_darker": round(sides - centre, 1)}


def judge(dump, log, fx):
    states = skin_judge.frame_states(log)
    want = fx.get("require_state")
    judged = 0
    red = []
    for present in fx["presents"]:
        path = os.path.join(dump, "frame_%06d.ppm" % present)
        state = states.get(present, "?")
        if not os.path.exists(path):
            print(json.dumps({"present": present, "verdict": "RED", "why": "no frame dumped"}))
            red.append(present)
            continue
        if want and state != want:
            print(json.dumps({"present": present, "state": state, "verdict": "skipped"}))
            continue
        m = measure(path, fx)
        if m is None:
            print(json.dumps({"present": present, "verdict": "RED", "why": "frame is not %dx%d" % tuple(fx["frame"])}))
            red.append(present)
            continue
        fails = []
        d = m["ring_minus_feet"]
        if d < fx["foot_darker_min"]:
            fails.append("no footprint: ring - feet %.1f < %.1f" % (d, fx["foot_darker_min"]))
        if d > fx["foot_darker_max"]:
            fails.append("the footprint is a stamp: ring - feet %.1f > %.1f" % (d, fx["foot_darker_max"]))
        r, g, b = m["body"]
        if not (r >= fx["fur_red_min"] and g <= 0.6 * r and b <= 0.25 * r):
            fails.append("no fur over the footprint: body (%.1f %.1f %.1f)" % (r, g, b))
        if m["white_run"] >= fx["white_run_max"]:
            fails.append("white streak: run %d at column %d" % (m["white_run"], m["white_column"]))
        if m["dark_run"] >= fx["dark_run_max"]:
            fails.append("dark streak / stamp: run %d at column %d" % (m["dark_run"], m["dark_column"]))
        if m["band_darker"] > fx["streak_max_darker"]:
            fails.append("the band above Crash is %.1f darker than its sides (max %.1f)" % (m["band_darker"], fx["streak_max_darker"]))
        m.update({"present": present, "state": state, "verdict": "RED" if fails else "GREEN"})
        if fails:
            m["why"] = "; ".join(fails)
            red.append(present)
        judged += 1
        print(json.dumps(m))
    if judged < 3:
        print("shadow_judge: RED -- only %d presents judged" % judged)
        return 1
    print("shadow_judge: %s (%d judged, %d red)" % ("GREEN" if not red else "RED", judged, len(red)))
    return 0 if not red else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory (with --log)")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/shadow_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        if not a.log:
            ap.error("--dir needs --log")
        return judge(a.dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir and --log)")
    out = a.out or os.path.join(ROOT, "build", "shadow_judge")
    dump, log, rc = skin_judge.run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("shadow_judge: RED -- the run exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
