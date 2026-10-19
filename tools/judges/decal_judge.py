#!/usr/bin/env python3
"""decal_judge.py -- are the DECALS drawn (Crash's footprints on the hub's sand) and do they FADE?

fixtures/judges/decal_footprints_pal.json replays fixtures/padlogs/decal_footprints.padlog (the hub menu prefix, then the left stick
forward for 50 boundaries from 2660 -- a few steps across the sand -- then neutral: Crash stands and the camera settles behind
him) TWICE on the same binary, deterministic and headless with frames dumped every `dump_every` presents: the DECAL arm as it
is, and the REFERENCE arm with the decal pass off (TE_GFX_NO_DECALS=1 -- DrawDecals records nothing; everything else, the game's
decal state included, is identical). The footprint the camera keeps in view lies in `box` (frame fractions, under Crash's
heels); per present the judge takes the reference's luminance minus the decal arm's over the box (positive = the decal darkens
the sand). It must hold:

  at `visible` (the footprint fully there, before its fade):  changed pixels (delta > `changed_delta`) >= min_changed,
                                                              max delta >= min_max_delta, mean delta >= min_mean_delta,
                                                              the reference box lit (mean >= min_reference_mean: sand, not a
                                                              black or missing frame)
  at `fading` (late in the footprint's 5 s life):             0 < max delta <= max_fading_ratio x the max delta at `visible`
                                                              (the decal is still there, fainter: AgeDecal's colour keys)
  at `gone` (after its life):                                 changed pixels <= max_gone_changed (the decal was dropped)

Thresholds are FROZEN in the fixture: RED on bin/twinsanityevolved.32e1174 (no decal path: the two arms are identical, delta 0
everywhere), GREEN on the decal branch (visible: 117 changed, max 22, mean 3.6; fading at 2760: max 13; gone at 2800: 0).

Usage: decal_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       decal_judge.py --dir DUMP --ref-dir DUMP --log run.log [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = a run failed / no frames.
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
import skin_judge  # noqa: E402  (read_ppm / frame_states)

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "decal_footprints_pal.json")


def run(binary, iso, out, fx, decals):
    """One arm: `decals` False = the reference (TE_GFX_NO_DECALS=1). Returns (dump dir, log path, exit code)."""
    arm = os.path.join(out, "decals" if decals else "reference")
    os.makedirs(arm, exist_ok=True)
    mc = os.path.join(arm, "mc")
    dump = os.path.join(arm, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    log = os.path.join(arm, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    env.pop("TE_GFX_NO_DECALS", None)
    if not decals:
        env["TE_GFX_NO_DECALS"] = "1"
    print("decal_judge: running (%s arm)" % ("decal" if decals else "reference"), " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("decal_judge: exit", rc, "->", log, flush=True)
    return dump, log, rc


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


def measure(dump, ref, present, fx):
    paths = [os.path.join(d, "frame_%06d.ppm" % present) for d in (dump, ref)]
    if not all(os.path.exists(p) for p in paths):
        return None
    now = box_luminances(paths[0], fx["box"], fx["frame"])
    before = box_luminances(paths[1], fx["box"], fx["frame"])
    if now is None or before is None:
        return None
    deltas = [b - a for a, b in zip(now, before)]
    n = len(deltas)
    return {"present": present, "reference_mean": round(sum(before) / n, 1), "decal_mean": round(sum(now) / n, 1),
            "mean_delta": round(sum(deltas) / n, 2), "max_delta": round(max(deltas), 1),
            "changed": sum(1 for d in deltas if d > fx["changed_delta"]), "pixels": n}


def judge(dump, ref, log, fx):
    states = skin_judge.frame_states(log)
    want = fx.get("require_state")
    fails = []
    results = {}
    for name in ("visible", "fading", "gone"):
        present = fx[name]
        m = measure(dump, ref, present, fx)
        if m is None:
            fails.append("%s: frame %d not dumped in both arms (or not %dx%d)" % (name, present, fx["frame"][0], fx["frame"][1]))
            continue
        m["state"] = states.get(present, "?")
        if want and m["state"] != want:
            fails.append("%s: present %d is in state %s, not %s" % (name, present, m["state"], want))
        results[name] = m
        print(json.dumps({name: m}))
    v = results.get("visible")
    if v:
        if v["reference_mean"] < fx["min_reference_mean"]:
            fails.append("visible: the reference box is not lit sand (mean %.1f < %.1f)" % (v["reference_mean"], fx["min_reference_mean"]))
        if v["changed"] < fx["min_changed"]:
            fails.append("visible: %d changed pixels < %d (no decal drawn)" % (v["changed"], fx["min_changed"]))
        if v["max_delta"] < fx["min_max_delta"]:
            fails.append("visible: max delta %.1f < %.1f" % (v["max_delta"], fx["min_max_delta"]))
        if v["mean_delta"] < fx["min_mean_delta"]:
            fails.append("visible: mean delta %.2f < %.2f" % (v["mean_delta"], fx["min_mean_delta"]))
    f = results.get("fading")
    if v and f:
        if not (0 < f["max_delta"] <= fx["max_fading_ratio"] * v["max_delta"]):
            fails.append("fading: max delta %.1f is not within (0, %.2f x %.1f] (the decal does not fade)" % (f["max_delta"], fx["max_fading_ratio"], v["max_delta"]))
    g = results.get("gone")
    if g and g["changed"] > fx["max_gone_changed"]:
        fails.append("gone: %d changed pixels > %d (the decal did not end)" % (g["changed"], fx["max_gone_changed"]))
    verdict = "RED" if fails else "GREEN"
    print(json.dumps({"verdict": verdict, "why": "; ".join(fails)} if fails else {"verdict": verdict}))
    print("decal_judge: %s" % verdict)
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing decal-arm dump directory (with --ref-dir and --log)")
    ap.add_argument("--ref-dir", help="the reference arm's dump directory")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/decal_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        if not a.log or not a.ref_dir:
            ap.error("--dir needs --ref-dir and --log")
        return judge(a.dir, a.ref_dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir, --ref-dir and --log)")
    out = a.out or os.path.join(ROOT, "build", "decal_judge")
    dump, log, rc = run(a.bin, a.iso, out, fx, True)
    if rc != 0:
        print("decal_judge: RED -- the decal arm exited %d" % rc, file=sys.stderr)
        return 2
    ref, _, rc = run(a.bin, a.iso, out, fx, False)
    if rc != 0:
        print("decal_judge: RED -- the reference arm exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, ref, log, fx)


if __name__ == "__main__":
    sys.exit(main())
