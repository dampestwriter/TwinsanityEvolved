#!/usr/bin/env python3
"""dust_judge.py -- is the LANDING DUST semi-transparent (the particle page's own alpha blend), not a solid dark blob?

fixtures/judges/landing_dust_pal.json replays fixtures/padlogs/landing_dust.padlog (the hub menu prefix, then two standing
jumps on the hub's sand at boundaries 2900 and 3100) deterministic and headless with frames dumped every `dump_every` presents.
For each landing in the fixture -- the present the dust first covers the ground beside Crash, plus its `reference` present a few
dumps earlier where he is still in the air and the ground beside him is bare -- the two ring boxes left and right of him (frame
fractions; the dust cloud without his body) must satisfy, in BOTH boxes:

  p05 (the landing frame's 5th-percentile luminance) >= min_floor_ratio x the reference box's mean luminance
        -- the darkest 5 % of the dust still show >= 40 % of the ground: a mean alpha contribution under 60 %
  dark share (pixels under min_floor_ratio x the reference mean) <= max_dark_share
  mean (landing) <= max_present_ratio x mean (reference)   -- SOMETHING is drawn there (an absent dust cannot fake GREEN)

A present whose GameController state is not `require_state` is skipped; fewer than 2 landings judged is RED. Thresholds are
FROZEN in the fixture: RED on bin/twinsanityevolved.c85ae9e.HELD (the mode-2 systems drawn with the SUBTRACTIVE material: p05 0-10,
dark share 0.17-0.31), GREEN on the landing-dust fix (p05 108-119 = 0.75-0.83 of the ground, dark share <= 0.03).

Usage: dust_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       dust_judge.py --dir DUMP --log run.log [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / no frames.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import skin_judge  # noqa: E402  (run / read_ppm / frame_states)

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "landing_dust_pal.json")


def box_luminances(path, box, frame):
    w, h, px = skin_judge.read_ppm(path)
    if [w, h] != list(frame):
        return None
    x0, y0, x1, y1 = box
    x0, x1 = int(round(x0 * w)), int(round(x1 * w))
    y0, y1 = int(round(y0 * h)), int(round(y1 * h))
    out = []
    for y in range(y0, y1):
        row = (y * w) * 3
        for x in range(x0, x1):
            i = row + x * 3
            out.append(0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2])
    return out


def percentile(values, p):
    s = sorted(values)
    if not s:
        return 0.0
    k = min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))
    return s[k]


def judge(dump, log, fx):
    states = skin_judge.frame_states(log)
    want = fx.get("require_state")
    floor = float(fx["min_floor_ratio"])
    judged, red = 0, []
    for landing in fx["landings"]:
        present, reference = landing["present"], landing["reference"]
        paths = [os.path.join(dump, "frame_%06d.ppm" % p) for p in (present, reference)]
        if not all(os.path.exists(p) for p in paths):
            print(json.dumps({"present": present, "verdict": "RED", "why": "frame %d or %d not dumped" % (present, reference)}))
            red.append(present)
            continue
        state = states.get(present, "?")
        if want and state != want:
            print(json.dumps({"present": present, "state": state, "verdict": "skipped"}))
            continue
        fails, measures = [], {"present": present, "reference": reference, "state": state, "boxes": []}
        for name, box in fx["ring_boxes"].items():
            now = box_luminances(paths[0], box, fx["frame"])
            before = box_luminances(paths[1], box, fx["frame"])
            if now is None or before is None:
                fails.append("frame is not %dx%d" % tuple(fx["frame"]))
                break
            ref_mean = sum(before) / len(before)
            mean = sum(now) / len(now)
            p05 = percentile(now, 5)
            dark = sum(1 for v in now if v < floor * ref_mean) / len(now)
            m = {"box": name, "reference_mean": round(ref_mean, 1), "mean": round(mean, 1), "mean_ratio": round(mean / ref_mean, 3),
                 "p05": round(p05, 1), "p05_ratio": round(p05 / ref_mean, 3), "dark_share": round(dark, 3)}
            measures["boxes"].append(m)
            if p05 < floor * ref_mean:
                fails.append("%s: the darkest dust is %.2f of the ground (p05 %.0f of %.0f; min %.2f)" % (name, p05 / ref_mean, p05, ref_mean, floor))
            if dark > fx["max_dark_share"]:
                fails.append("%s: dark share %.3f > %.3f" % (name, dark, fx["max_dark_share"]))
            if mean > fx["max_present_ratio"] * ref_mean:
                fails.append("%s: no dust drawn (mean %.2f of the ground > %.2f)" % (name, mean / ref_mean, fx["max_present_ratio"]))
        measures["verdict"] = "RED" if fails else "GREEN"
        if fails:
            measures["why"] = "; ".join(fails)
            red.append(present)
        judged += 1
        print(json.dumps(measures))
    if judged < 2:
        print("dust_judge: RED -- only %d landings judged" % judged)
        return 1
    print("dust_judge: %s (%d landings judged, %d red)" % ("GREEN" if not red else "RED", judged, len(red)))
    return 0 if not red else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory (with --log)")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/dust_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        if not a.log:
            ap.error("--dir needs --log")
        return judge(a.dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir and --log)")
    out = a.out or os.path.join(ROOT, "build", "dust_judge")
    dump, log, rc = skin_judge.run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("dust_judge: RED -- the run exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
