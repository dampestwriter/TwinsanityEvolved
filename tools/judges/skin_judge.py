#!/usr/bin/env python3
"""skin_judge.py -- are the skinned characters DRAWN AND TEXTURED? The frame region Crash stands in at the hub
(fixtures/judges/hub_skin_pal.json: the hub pad log, deterministic headless, frames dumped every 100 presents) must hold a
textured character, not sand plus a black silhouette: on each judged Playing present the region's

  colour count (RGB quantised to `quantise_bits`)              >= min_colours
  luminance SD of the pixels that are not near-black           >= min_lit_luminance_sd   (the fur's shading; the shadow
                                                                                            stamp's black is excluded, so it
                                                                                            cannot fake variance)
  share of strongly saturated pixels (max-min >= min_saturation) >= min_saturated_share   (orange fur; sand and the stamp
                                                                                            are not saturated)

A present whose GameController state is not `require_state` is skipped; fewer than 3 judged presents is RED. Thresholds
are FROZEN in the fixture (red on bin/twinsanityevolved.1952747, green on the skins fix).

Usage: skin_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       skin_judge.py --dir DUMP --log run.log [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / no frames.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_skin_pal.json")
FRAME = re.compile(r"\[te\.gfx\] frame (\d+) -> \S+")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    dump = os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    print("skin_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("skin_judge: exit", rc, "->", log, flush=True)
    return dump, log, rc


def frame_states(log_path):
    states = {}
    state = "?"
    with open(log_path, errors="replace") as f:
        for line in f:
            m = STATE.search(line)
            if m:
                state = m.group(2)
                continue
            m = FRAME.search(line)
            if m:
                states[int(m.group(1))] = state
    return states


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: magic, width, height, maxval, then binary RGB
    parts = []
    pos = 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    if parts[0] != b"P6":
        raise ValueError("not a P6 ppm: " + path)
    w, h = int(parts[1]), int(parts[2])
    return w, h, memoryview(data)[pos:pos + 3 * w * h]


def measure(path, fx):
    w, h, px = read_ppm(path)
    x0, y0, x1, y1 = fx["region"]["box"]
    x0, x1 = int(round(x0 * w)), int(round(x1 * w))
    y0, y1 = int(round(y0 * h)), int(round(y1 * h))
    shift = 8 - int(fx["quantise_bits"])
    dark = float(fx["dark_luminance"])
    min_sat = int(fx["min_saturation"])
    colours = set()
    lit = []
    saturated = 0
    n = 0
    for y in range(y0, y1):
        row = (y * w) * 3
        for x in range(x0, x1):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            colours.add((r >> shift, g >> shift, b >> shift))
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            n += 1
            if lum >= dark:
                lit.append(lum)
                if max(r, g, b) - min(r, g, b) >= min_sat:
                    saturated += 1
    sd = statistics.pstdev(lit) if len(lit) > 1 else 0.0
    return {"colours": len(colours), "lit_luminance_sd": round(sd, 1), "saturated_share": round(saturated / n, 3) if n else 0.0,
            "pixels": n, "lit_pixels": len(lit)}


def judge(dump, log, fx):
    states = frame_states(log)
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
        fails = []
        if m["colours"] < fx["min_colours"]:
            fails.append("colours %d < %d" % (m["colours"], fx["min_colours"]))
        if m["lit_luminance_sd"] < fx["min_lit_luminance_sd"]:
            fails.append("lit luminance sd %.1f < %.1f" % (m["lit_luminance_sd"], fx["min_lit_luminance_sd"]))
        if m["saturated_share"] < fx["min_saturated_share"]:
            fails.append("saturated share %.3f < %.3f" % (m["saturated_share"], fx["min_saturated_share"]))
        m.update({"present": present, "state": state, "region": fx["region"]["name"], "verdict": "RED" if fails else "GREEN"})
        if fails:
            m["why"] = "; ".join(fails)
            red.append(present)
        judged += 1
        print(json.dumps(m))
    if judged < 3:
        print("skin_judge: RED -- only %d presents judged" % judged)
        return 1
    print("skin_judge: %s (%d judged, %d red)" % ("GREEN" if not red else "RED", judged, len(red)))
    return 0 if not red else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory (with --log)")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/skin_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        if not a.log:
            ap.error("--dir needs --log")
        return judge(a.dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir and --log)")
    out = a.out or os.path.join(ROOT, "build", "skin_judge")
    dump, log, rc = run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("skin_judge: RED -- the run exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
