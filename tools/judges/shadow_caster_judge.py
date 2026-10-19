#!/usr/bin/env python3
"""shadow_caster_judge.py -- is the shadow composite kept off the caster? The RootShadowLevelEntry criteria
extended to the caster's body on the port's PAL hub frame at present 2900 (fixtures/judges/hub_shadow_caster_pal.json;
the hub pad log, deterministic headless, frames dumped every 100 presents). The retail StartShadows counted a volume strip
only where the frame's alpha top bit was 0 (DATE against the shrunk frame alpha): the lit world (alpha 0x7E) receives the
shadows, a skin (alpha 0x80) never does. The port counted the caster too, so his own volume darkened half his body. On
each judged Playing present:

  torso      mean luminance of the torso box >= torso_lum_min (the shadows-off reference minus a small delta), and at
             most torso_dark_frac_max of its pixels near-black (max(r,g,b) < torso_dark_max_channel): no blotch over him
  legs       mean luminance of the legs box >= legs_lum_min
  footprint  foot_darker_min <= ring - foot <= foot_darker_max: the sand beside his shoes still carries the shadow (a
             quarter darkening, not a stamp) -- a build that drops the composite altogether is RED too

A present whose GameController state is not `require_state` is skipped; no judged present is RED. Thresholds are FROZEN
in the fixture (red on bin/twinsanityevolved.c85ae9e.HELD, green on the DATE-gated count).

Usage: shadow_caster_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       shadow_caster_judge.py --dir DUMP --log run.log [--fixture F]
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
from shadow_judge import lum  # noqa: E402

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_shadow_caster_pal.json")


def dark_fraction(px, w, box, max_channel):
    x0, y0, x1, y1 = box
    dark = 0
    n = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            if max(px[i], px[i + 1], px[i + 2]) < max_channel:
                dark += 1
            n += 1
    return dark / n if n else 0.0


def measure(path, fx):
    w, h, px = skin_judge.read_ppm(path)
    if [w, h] != fx["frame"]:
        return None
    foot = lum(px, w, fx["foot_box"])
    ring = sum(lum(px, w, b) for b in fx["ring_boxes"]) / len(fx["ring_boxes"])
    return {"torso_lum": round(lum(px, w, fx["torso_box"]), 1),
            "torso_dark_frac": round(dark_fraction(px, w, fx["torso_box"], fx["torso_dark_max_channel"]), 3),
            "legs_lum": round(lum(px, w, fx["legs_box"]), 1),
            "ring_minus_foot": round(ring - foot, 1)}


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
        if m["torso_lum"] < fx["torso_lum_min"]:
            fails.append("the shadow is over the caster: torso %.1f < %.1f" % (m["torso_lum"], fx["torso_lum_min"]))
        if m["torso_dark_frac"] > fx["torso_dark_frac_max"]:
            fails.append("blotches on the torso: %.1f%% near-black > %.1f%%" % (100 * m["torso_dark_frac"], 100 * fx["torso_dark_frac_max"]))
        if m["legs_lum"] < fx["legs_lum_min"]:
            fails.append("the shadow is over the legs: %.1f < %.1f" % (m["legs_lum"], fx["legs_lum_min"]))
        d = m["ring_minus_foot"]
        if d < fx["foot_darker_min"]:
            fails.append("no footprint: ring - foot %.1f < %.1f" % (d, fx["foot_darker_min"]))
        if d > fx["foot_darker_max"]:
            fails.append("the footprint is a stamp: ring - foot %.1f > %.1f" % (d, fx["foot_darker_max"]))
        m.update({"present": present, "state": state, "verdict": "RED" if fails else "GREEN"})
        if fails:
            m["why"] = "; ".join(fails)
            red.append(present)
        judged += 1
        print(json.dumps(m))
    if judged < 1:
        print("shadow_caster_judge: RED -- no present judged")
        return 1
    print("shadow_caster_judge: %s (%d judged, %d red)" % ("GREEN" if not red else "RED", judged, len(red)))
    return 0 if not red else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory (with --log)")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/shadow_caster_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        if not a.log:
            ap.error("--dir needs --log")
        return judge(a.dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir and --log)")
    out = a.out or os.path.join(ROOT, "build", "shadow_caster_judge")
    dump, log, rc = skin_judge.run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("shadow_caster_judge: RED -- the run exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
