#!/usr/bin/env python3
"""particle_judge.py -- the particle blocks are DRAWN through the VU1 library's particle program (vs_zero_kick class C)
and the material shaders' UV scroll is stepped. The hub pad log (fixtures/judges/hub_particles_pal.json) replays headless
WITH draws to `stop_at`; the run's own summary line must report
  particle blocks >= min_blocks      (the sparkles / dust of the hub's first seconds of play: 1739 blocks by present 2240)
  particle triangles >= min_triangles
  particle refused == 0              (no block the program body declined)
and the diag of present `diag_present` must show at least `min_scrolling_draws` draws with a non-zero UV scroll (the sea's
water, StepAnimations) and at least `min_particle_draws` particle draws whose boxes lie inside the frame. A runner without the
particle counters (before the particle path) is RED. Thresholds FROZEN in the fixture: RED on bin/twinsanityevolved.1952747 (no counters,
every particle entry a no-op, StepAnimations a no-op), GREEN on the particle-path build.
Usage: particle_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       particle_judge.py --log run.log [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_particles_pal.json")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    os.makedirs(mc)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--mc-dir", mc, "--stop-at", str(fx["stop_at"]),
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DIAG_PRESENT=str(fx["diag_present"]))
    print("particle_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("particle_judge: exit", rc, "->", log, flush=True)
    return rc, log


def judge(text, fx):
    problems, measures = [], {}
    summaries = re.findall(r"particle blocks (\d+) \(triangles (\d+), refused (\d+)\)", text)
    if not summaries:
        problems.append("no particle counters on the summary line (a runner without the particle path)")
    else:
        blocks, tris, refused = (int(x) for x in summaries[-1])
        measures.update(particle_blocks=blocks, particle_triangles=tris, particle_refused=refused)
        if blocks < fx["min_blocks"]:
            problems.append(f"particle blocks {blocks} < {fx['min_blocks']}")
        if tris < fx["min_triangles"]:
            problems.append(f"particle triangles {tris} < {fx['min_triangles']}")
        if refused:
            problems.append(f"{refused} particle blocks refused by the program body")
    w, h = fx["frame"]
    present = fx["diag_present"]
    boxes = re.findall(rf"present {present} particles block \S+ .* tris (\d+) box \((-?\d+) (-?\d+)\)-\((-?\d+) (-?\d+)\)", text)
    inside = [b for b in boxes if int(b[0]) > 0 and int(b[1]) >= -w and int(b[3]) <= 2 * w and int(b[2]) >= -h and int(b[4]) <= 2 * h
              and int(b[3]) > 0 and int(b[1]) < w and int(b[4]) > 0 and int(b[2]) < h]
    measures["particle_draws_in_frame"] = len(inside)
    if len(inside) < fx["min_particle_draws"]:
        problems.append(f"{len(inside)} particle draws inside the frame at present {present} (wanted {fx['min_particle_draws']})")
    scrolls = re.findall(rf"present {present} .* scroll \(([-0-9.e]+) ([-0-9.e]+)\)", text)
    scrolling = [s for s in scrolls if abs(float(s[0])) > 1e-6 or abs(float(s[1])) > 1e-6]
    measures["scrolling_draws"] = len(scrolling)
    if len(scrolling) < fx["min_scrolling_draws"]:
        problems.append(f"{len(scrolling)} draws with a UV scroll at present {present} (wanted {fx['min_scrolling_draws']})")
    m = re.search(r"--stop-at (\d+) reached", text)
    if not m or int(m.group(1)) < fx["stop_at"]:
        problems.append(f"the run did not reach --stop-at {fx['stop_at']}")
    states = re.findall(r"game state @ boundary (\d+): (\w+) \(", text)
    measures["final_state"] = states[-1][1] if states else None
    if not states or states[-1][1] != fx["final_state"]:
        problems.append(f"final state {measures['final_state']} (wanted {fx['final_state']})")
    if re.search(r"wasm trap: ", text):
        problems.append("trap")
    return problems, measures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--log")
    ap.add_argument("--out", help="the run's directory (default: build/particle_judge)")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.log:
        log = a.log
    elif a.bin and a.iso:
        rc, log = run(a.bin, a.iso, a.out or os.path.join(ROOT, "build", "particle_judge"), fx)
        if rc != 0:
            print("particle_judge: RED -- the run exited %d" % rc, file=sys.stderr)
            return 2
    else:
        ap.error("--bin and --iso (or --log)")
    problems, measures = judge(open(log, errors="replace").read(), fx)
    print(json.dumps(measures))
    for p in problems:
        print("particle_judge:  " + p)
    print("particle_judge: %s" % ("GREEN" if not problems else "RED"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
