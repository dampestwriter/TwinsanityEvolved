#!/usr/bin/env python3
"""interpolation_quality_judge.py -- the frozen judge of the frames the presenter interpolates between two game pictures
at 120: each interpolated frame at fraction 0.5 against the PIXEL MEAN of its two real neighbours, SSIM on the scene
region (the HUD bands masked) on three scenes -- the hub (walking), the title's intro cutscene (the camera over the ocean)
and a Cavern Catastrophe run -- and the interpolator's own displacement check: zero matched objects whose step between
consecutive shown frames exceeded the step between the two real pictures (a wrong identity match).

    interpolation_quality_judge.py --bin BUILD/twinsanityevolved --iso <disc image> [--out DIR]
                                   [--fixture fixtures/judges/interpolation_quality_pal.json] [--only hub]

The run per scene: PLAY mode at `--frame-rate 120` with the render thread forced on (TE_GFX_RENDER_THREAD=1: the presenter
paces on the pacer's clock and interpolates), headless, the pictures of the scene's window dumped every one
(TE_GFX_DUMP_DIR / TE_GFX_DUMP_EVERY=1 / TE_GFX_DUMP_FROM / TE_GFX_DUMP_TO, by PICTURE number) and, at every picture's
arrival, the HALF-WAY frame of (previous, newest) built by the same interpolator and dumped beside them
(TE_GFX_INTERP_PROBE=1: frame_<picture>_i500.ppm -- the fraction-0.5 frame exactly, whatever the pacer's slots landed on
in this run). A triplet is (frame_<n-1>.ppm, frame_<n>_i500.ppm, frame_<n>.ppm). The presenter's own displacement census
(the live run's, every interpolated frame) is read off the exit line.

Judged (the fixture freezes the numbers): per scene the median SSIM over the triplets >= `min_median_ssim` (0.95) and the
minimum >= `min_ssim`; at least `min_triplets` triplets; the run exits 0 at the stop with no trap. Then a second, PACED leg
per scene (windowed, no dumps, TE_GFX_INTERP_CHECK=1: the presenter's own displacement check on): its census must carry
`displacement checks N violations V` with V / N within the fixture's `max_violation_rate` (2e-4: a wrong identity CLASS shows
as thousands; the residual is two instances of one model within the object threshold matched to each other's previous
instance by the 1:1 assignment); the headless probe leg lags the pacer under its per-frame dumps and is not judged on it. A runner without --frame-rate (the baseline: no interpolation) is RED. SSIM: luma, 8x8
blocks, the standard constants, the mean of the blocks inside the scene region (the window rows between the HUD bands'
fractions).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "interpolation_quality_pal.json")
PRESENT = re.compile(r"\] (present=[^ ]+)(.*)$")
DISP = re.compile(r"displacement checks (\d+) violations (\d+)")
STOP = re.compile(r"--stop-at (\d+) reached")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6":
        raise ValueError("not a P6 ppm: " + path)
    w, h = (int(x) for x in parts[1].split())
    pixels = np.frombuffer(parts[3], dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)
    return pixels


def luma(img):
    return 0.299 * img[:, :, 0].astype(np.float64) + 0.587 * img[:, :, 1] + 0.114 * img[:, :, 2]


def ssim_blocks(a, b, block=8):
    """Mean SSIM over block x block tiles of two luma images (the standard constants)."""
    h, w = a.shape
    h, w = h - h % block, w - w % block
    a = a[:h, :w].reshape(h // block, block, w // block, block).transpose(0, 2, 1, 3).reshape(-1, block * block)
    b = b[:h, :w].reshape(h // block, block, w // block, block).transpose(0, 2, 1, 3).reshape(-1, block * block)
    ma, mb = a.mean(axis=1), b.mean(axis=1)
    va, vb = a.var(axis=1), b.var(axis=1)
    cov = ((a - ma[:, None]) * (b - mb[:, None])).mean(axis=1)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(s.mean())


def prepare_card(row, out, hub_card):
    card = os.path.join(out, "mc")
    shutil.rmtree(card, ignore_errors=True)
    spec = row.get("card", "fresh")
    if spec.startswith("warp:"):
        if not hub_card or not os.path.isdir(hub_card):
            print("interpolation_quality_judge: %s needs the hub card (run the hub scene first)" % row["name"])
            return None
        rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", hub_card, "--chunk",
                              spec[len("warp:"):], "--out", card, "--place", "0xFFFF"], stdout=subprocess.DEVNULL, cwd=ROOT)
        if rc != 0:
            print("interpolation_quality_judge: warp_save.py failed for", row["name"])
            return None
    else:
        os.makedirs(card)
    return card


def run_scene(binary, iso, row, out, hub_card, fx):
    os.makedirs(out, exist_ok=True)
    card = prepare_card(row, out, hub_card)
    if card is None:
        return None, card
    dump = os.path.join(out, "frames")
    shutil.rmtree(dump, ignore_errors=True)
    os.makedirs(dump)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--frame-rate", "120", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, row["padlog"]), "--mc-dir", card, "--stop-at", str(row["stop_at"]),
            "--run-seconds", str(row.get("run_seconds", 300)), "--config", "none"]
    args += [a.replace("$ROOT", ROOT) for a in row.get("extra_args", [])]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_RENDER_THREAD"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = "1"
    env["TE_GFX_DUMP_FROM"] = str(row["pictures"][0])
    env["TE_GFX_DUMP_TO"] = str(row["pictures"][1])
    env["TE_GFX_INTERP_PROBE"] = "1"
    env["TE_GFX_INTERP_CHECK"] = "1"
    env.pop("TE_FRAME_RATE", None)
    print("interpolation_quality_judge: [%s] running %s" % (row["name"], " ".join(args)), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    return {"rc": rc, "log": log, "dump": dump}, card


def judge_scene(row, run, fx):
    m = {"name": row["name"], "exit": run["rc"], "problems": []}
    if run["rc"] == 2:
        m["problems"].append("the runner refused --frame-rate (no interpolation)")
        return m
    with open(run["log"], errors="replace") as f:
        for line in f:
            if STOP.search(line):
                m["stop_reached"] = True
            if "wasm trap" in line:
                m["trap"] = True
            pr = PRESENT.search(line)
            if pr and "[twinsanityevolved]" in line:
                m["present_row"] = pr.group(1)
                d = DISP.search(pr.group(2))
                if d:
                    m["displacement_checks"], m["displacement_violations"] = int(d.group(1)), int(d.group(2))
    if run["rc"] != 0:
        m["problems"].append("exit %d" % run["rc"])
    if m.get("trap"):
        m["problems"].append("a wasm trap")
    if not m.get("stop_reached"):
        m["problems"].append("--stop-at not reached")
    # the triplets
    files = os.listdir(run["dump"])
    pictures = {}
    interps = {}
    for name in files:
        mm = re.match(r"frame_(\d+)\.ppm$", name)
        if mm:
            pictures[int(mm.group(1))] = os.path.join(run["dump"], name)
            continue
        mm = re.match(r"frame_(\d+)_i(\d+)\.ppm$", name)
        if mm:
            interps.setdefault(int(mm.group(1)), []).append((int(mm.group(2)), os.path.join(run["dump"], name)))
    tol = int(fx["fraction_tolerance"] * 1000)
    rows0, rows1 = fx["scene_rows"]
    ssims = []
    for n in sorted(interps):
        if n - 1 not in pictures or n not in pictures:
            continue
        mids = [p for (fr, p) in interps[n] if abs(fr - 500) <= tol]
        if not mids:
            continue
        a = read_ppm(pictures[n - 1])
        b = read_ppm(pictures[n])
        i = read_ppm(mids[0])
        if a.shape != b.shape or a.shape != i.shape:
            continue
        h = a.shape[0]
        y0, y1 = int(h * rows0), int(h * rows1)
        mean = (luma(a)[y0:y1] + luma(b)[y0:y1]) * 0.5
        ssims.append((n, ssim_blocks(luma(i)[y0:y1], mean)))
    m["triplets"] = len(ssims)
    if ssims:
        values = sorted(s for (_, s) in ssims)
        m["ssim_median"] = round(values[len(values) // 2], 4)
        m["ssim_min"] = round(values[0], 4)
        m["ssim_min_picture"] = min(ssims, key=lambda t: t[1])[0]
        m["ssim_below_min_median"] = sum(1 for v in values if v < fx["min_median_ssim"])
    if m["triplets"] < fx["min_triplets"]:
        m["problems"].append("only %d triplets (< %d)" % (m["triplets"], fx["min_triplets"]))
    if m.get("ssim_median") is not None and m["ssim_median"] < fx["min_median_ssim"]:
        m["problems"].append("median SSIM %.4f < %s" % (m["ssim_median"], fx["min_median_ssim"]))
    if m.get("ssim_min") is not None and m["ssim_min"] < fx["min_ssim"]:
        m["problems"].append("minimum SSIM %.4f < %s (picture %d)" % (m["ssim_min"], fx["min_ssim"], m["ssim_min_picture"]))
    return m


def run_paced(binary, iso, row, out, card, fx):
    """The paced leg: windowed at 120 with the displacement check on, no dumps; the census's violations must be 0."""
    log = os.path.join(out, "paced.log")
    mc = os.path.join(out, "mc_paced")
    shutil.rmtree(mc, ignore_errors=True)
    shutil.copytree(card, mc)
    args = [binary, "--iso", iso, "--frame-rate", "120", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, row["padlog"]), "--mc-dir", mc, "--stop-at", str(row["stop_at"]),
            "--run-seconds", str(row.get("run_seconds", 300)), "--config", "none"]
    args += [a.replace("$ROOT", ROOT) for a in row.get("extra_args", [])]
    env = dict(os.environ)
    for k in ("TE_GFX_HEADLESS", "TE_GFX_DUMP_DIR", "TE_GFX_DUMP_EVERY", "TE_GFX_INTERP_PROBE", "TE_FRAME_RATE"):
        env.pop(k, None)
    env["TE_GFX_INTERP_CHECK"] = "1"
    print("interpolation_quality_judge: [%s] paced leg: %s" % (row["name"], " ".join(args)), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    m = {"paced_exit": rc}
    with open(log, errors="replace") as f:
        for line in f:
            pr = PRESENT.search(line)
            if pr and "[twinsanityevolved]" in line:
                m["paced_present_row"] = pr.group(1)
                d = DISP.search(pr.group(2))
                if d:
                    m["paced_displacement_checks"], m["paced_displacement_violations"] = int(d.group(1)), int(d.group(2))
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--only", default=None, help="one scene by name")
    ap.add_argument("--keep-frames", action="store_true", help="keep the dumped frames (deleted after the measure by default)")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    out = a.out or os.path.join(ROOT, "build", "interpolation_quality_judge")
    os.makedirs(out, exist_ok=True)
    hub_card = None
    results = []
    for row in fx["scenes"]:
        if a.only and row["name"] != a.only:
            continue
        run, card = run_scene(a.bin, a.iso, row, os.path.join(out, row["name"]), hub_card, fx)
        if run is None:
            results.append({"name": row["name"], "problems": ["the scene's card could not be prepared"]})
            continue
        if row.get("card", "fresh") == "fresh" and row["name"] == "hub":
            hub_card = card
        m = judge_scene(row, run, fx)
        if run["rc"] == 0:
            pm = run_paced(a.bin, a.iso, row, os.path.join(out, row["name"]), card, fx)
            m.update(pm)
            if pm.get("paced_exit") != 0:
                m["problems"].append("the paced leg exited %s" % pm.get("paced_exit"))
            elif pm.get("paced_displacement_violations") is None:
                m["problems"].append("the paced leg has no displacement census (the interpolator did not run)")
            elif pm["paced_displacement_violations"] > fx.get("max_violation_rate", 0.0) * max(1, pm["paced_displacement_checks"]):
                m["problems"].append("%d displacement violations over %d checks on the paced leg (> %g of the checks)" % (
                    pm["paced_displacement_violations"], pm["paced_displacement_checks"], fx.get("max_violation_rate", 0.0)))
        results.append(m)
        print("interpolation_quality_judge: [%s] exit %s, %s triplets, SSIM median %s min %s (picture %s); paced leg: displacement checks %s violations %s, %s" % (
            m["name"], m.get("exit"), m.get("triplets"), m.get("ssim_median"), m.get("ssim_min"), m.get("ssim_min_picture"),
            m.get("paced_displacement_checks"), m.get("paced_displacement_violations"), m.get("paced_present_row")), flush=True)
        if not a.keep_frames and run.get("dump"):
            shutil.rmtree(run["dump"], ignore_errors=True)
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump(results, f, indent=2)
    problems = ["[%s] %s" % (m["name"], p) for m in results for p in m["problems"]]
    if problems:
        for p in problems:
            print("interpolation_quality_judge:", p)
        print("interpolation_quality_judge: RED (%d problem(s))" % len(problems))
        return 1
    print("interpolation_quality_judge: GREEN (%s)" % "; ".join("%s SSIM median %.4f min %.4f over %d triplets, %d violations over %d checks on the paced leg" % (
        m["name"], m["ssim_median"], m["ssim_min"], m["triplets"], m.get("paced_displacement_violations", 0), m.get("paced_displacement_checks", 0)) for m in results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
