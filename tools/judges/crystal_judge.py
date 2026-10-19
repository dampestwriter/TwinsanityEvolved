#!/usr/bin/env python3
"""crystal_judge.py -- the Cavern Catastrophe puzzle crystals' COLOUR against the oracle.

The cryscave's three "touch in order" crystals are dual-shader materials: [type 0x10 screen copy] [type 0x02 lit] -- the
retail VU1 crystal program (vs_crys_dual) draws the refraction of the frame behind the crystal first and the lit, alpha-
blended, UV-scrolled colour-band texture (red / green / yellow) over it. The port drew a material's FIRST shader only, so
the three crystals were the lights' lavender (vertex colour white x the cave's lights) with no band: colourless against
the retail picture. The judge measures, in fixed boxes of fixed presented frames of the deterministic cryscave pad log,
the mean HSV saturation of the crystal's bright pixels (V >= v_floor: the crystal against the dark cave) and its VIVID
share (S >= vivid_s): each judged crystal must reach the fixture's floors, and when an oracle frame is given (the recomp
oracle's frame at the matching state, or a PCSX2 frame dump) the port's mean saturation must lie within `oracle_tol` of
the oracle's for that crystal.

Usage: crystal_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR] [--oracle-frame PPM --oracle-box x0,y0,x1,y1 [--oracle-crystal NAME]]
       crystal_judge.py --dir DUMPDIR [--fixture F] [...]            judge an existing dump
Exit 0 = GREEN, 1 = RED, 2 = the run failed / nothing to judge.
"""
import argparse
import colorsys
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "cavern_crystals_pal.json")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc, dump = os.path.join(out, "mc"), os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    if fx.get("card", "fresh").startswith("warp:"):
        hub = os.path.join(out, "mc_hub")
        shutil.rmtree(hub, ignore_errors=True)
        os.makedirs(hub)
        env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
        subprocess.call([binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
                         "--play-pad", os.path.join(ROOT, "fixtures/padlogs/hub_entry.padlog"), "--mc-dir", hub, "--stop-at", "2700"],
                        stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "hub_card.log"), "w"), env=env, cwd=ROOT)
        shutil.rmtree(mc, ignore_errors=True)
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools/warp_save.py"), "--card", hub, "--chunk", fx["card"][5:],
                               "--out", mc, "--place", "0xFFFF"], stdout=subprocess.DEVNULL, cwd=ROOT)
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DUMP_DIR=dump, TE_GFX_DUMP_EVERY=str(fx["dump_every"]))
    log = os.path.join(out, "run.log")
    print("crystal_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("crystal_judge: exit", rc, "->", log, flush=True)
    wanted = {c["present"] for c in fx["crystals"]}
    for n in os.listdir(dump):
        m = re.match(r"frame_(\d+)\.ppm$", n)
        if m and int(m.group(1)) not in wanted:
            os.remove(os.path.join(dump, n))
    return dump, rc


def read_image(path, flipped=False):
    """RGB rows of a binary PPM (P6, 8-bit) or, with Pillow available, any image; `flipped` = the rows are bottom-up (the
    recomp oracle's --dump-frames) and are turned top-down."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if m:
        w, h = int(m.group(1)), int(m.group(2))
        px = data[m.end():m.end() + w * h * 3]
    else:
        from PIL import Image  # a PNG / BMP from PCSX2
        im = Image.open(path).convert("RGB")
        w, h, px = im.width, im.height, im.tobytes()
    if flipped:
        row = w * 3
        px = b"".join(px[(h - 1 - y) * row:(h - y) * row] for y in range(h))
    return w, h, px


def saturation(image, box, v_floor, vivid_s):
    """Over the box's pixels with HSV value >= v_floor: the count, the mean saturation and the share with S >= vivid_s."""
    w, h, px = image
    x0, y0, x1, y1 = box
    x0, y0 = max(0, int(x0)), max(0, int(y0))
    x1, y1 = min(w, int(x1)), min(h, int(y1))
    n = bright = vivid = 0
    s_sum = 0.0
    for y in range(y0, y1):
        row = (y * w) * 3
        for x in range(x0, x1):
            i = row + x * 3
            r, g, b = px[i] / 255.0, px[i + 1] / 255.0, px[i + 2] / 255.0
            n += 1
            v = max(r, g, b)
            if v < v_floor:
                continue
            s = 0.0 if v == 0 else (v - min(r, g, b)) / v
            bright += 1
            s_sum += s
            if s >= vivid_s:
                vivid += 1
    return {"pixels": n, "bright": bright, "mean_s": (s_sum / bright) if bright else 0.0, "vivid_share": (vivid / bright) if bright else 0.0}


def judge(dump, fx, oracle=None):
    v_floor, vivid_s = fx.get("v_floor", 0.35), fx.get("vivid_s", 0.6)
    reasons, rows = [], []
    for c in fx["crystals"]:
        path = os.path.join(dump, "frame_%06d.ppm" % c["present"])
        if not os.path.exists(path):
            reasons.append("%s: present %d not dumped" % (c["name"], c["present"]))
            continue
        st = saturation(read_image(path), c["box"], v_floor, vivid_s)
        row = {"name": c["name"], "present": c["present"], "bright": st["bright"], "mean_s": round(st["mean_s"], 3),
               "vivid_share": round(st["vivid_share"], 3)}
        if st["bright"] < c.get("min_bright", 50):
            reasons.append("%s: %d bright pixels < %d (the crystal is not in its box)" % (c["name"], st["bright"], c.get("min_bright", 50)))
        if c.get("judged", True):
            if st["mean_s"] < c["min_mean_s"]:
                reasons.append("%s: mean saturation %.3f < %.3f (colourless)" % (c["name"], st["mean_s"], c["min_mean_s"]))
            if st["vivid_share"] < c["min_vivid_share"]:
                reasons.append("%s: vivid share %.3f < %.3f" % (c["name"], st["vivid_share"], c["min_vivid_share"]))
        if oracle and oracle["crystal"] == c["name"]:
            ost = saturation(read_image(oracle["frame"], oracle.get("flipped", False)), oracle["box"], v_floor, vivid_s)
            row["oracle_mean_s"] = round(ost["mean_s"], 3)
            row["oracle_vivid_share"] = round(ost["vivid_share"], 3)
            row["oracle_bright"] = ost["bright"]
            tol = fx.get("oracle_tol", 0.15)
            if ost["bright"] < 20:
                reasons.append("%s: the oracle's box has %d bright pixels (no crystal there)" % (c["name"], ost["bright"]))
            elif abs(st["mean_s"] - ost["mean_s"]) > tol:
                reasons.append("%s: mean saturation %.3f vs the oracle's %.3f (tolerance %.2f)" % (c["name"], st["mean_s"], ost["mean_s"], tol))
        rows.append(row)
    for r in rows:
        print("crystal_judge:", json.dumps(r))
    if not rows:
        print("crystal_judge: nothing to judge")
        return 2
    if reasons:
        for r in reasons:
            print("  RED:", r)
        print("crystal_judge: RED")
        return 1
    print("crystal_judge: GREEN (%d crystals)" % len(rows))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: build/crystal_judge)")
    ap.add_argument("--oracle-frame", help="the oracle's frame at the matching state (PPM, or PNG/BMP with Pillow): the recomp oracle's dump or a PCSX2 frame dump")
    ap.add_argument("--oracle-box", help="x0,y0,x1,y1 of that crystal in the oracle's frame (default: the fixture's oracle box)")
    ap.add_argument("--oracle-crystal", help="which fixture crystal the oracle frame shows (default: the fixture's)")
    ap.add_argument("--oracle-flipped", action="store_true", help="the oracle frame's rows are bottom-up (the recomp oracle's dumps; default: the fixture's flag, false for a PCSX2 dump)")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    oracle = None
    ofx = fx.get("oracle", {})
    frame = a.oracle_frame or ofx.get("frame")
    if frame:
        if not os.path.isabs(frame):
            frame = os.path.join(ROOT, frame)
        box = [int(v) for v in a.oracle_box.split(",")] if a.oracle_box else ofx.get("box")
        if not os.path.exists(frame) or not box:
            print("crystal_judge: oracle frame %s missing or no box -- the oracle check is skipped" % frame)
        else:
            oracle = {"frame": frame, "box": box, "crystal": a.oracle_crystal or ofx.get("crystal"),
                      "flipped": a.oracle_flipped or (not a.oracle_frame and ofx.get("flipped", False))}
            print("crystal_judge: oracle frame", frame, "box", box, "flipped", oracle["flipped"])
    if a.dir:
        return judge(a.dir, fx, oracle)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir)")
    out = a.out or os.path.join(ROOT, "build", "crystal_judge")
    dump, rc = run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("crystal_judge: the run exited %d" % rc)
    return judge(dump, fx, oracle)


if __name__ == "__main__":
    sys.exit(main())
