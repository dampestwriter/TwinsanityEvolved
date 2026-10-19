#!/usr/bin/env python3
"""oracle_walk_judge.py -- the frozen judge of the port's game time against the oracle's: on the minimal
analog walk (fixtures/padlogs/hub_walk_forward.padlog: the Jungle Bungle menu prefix, then the left stick held full forward
from boundary 1950) the port's Crash must stand where the oracle's stands at every matched boundary of the leap-free window,
and the game must take retail's 149 clock units a frame (TimeClockAdvanceBy under the EE's round-toward-zero arithmetic),
not 150.

    oracle_walk_judge.py --bin BUILD/twinsanityevolved --iso <PAL disc image> [--out DIR] [--fixture fixtures/judges/hub_walk_forward_oracle.json]
                         [--ee-rounding]      # run the port with TE_EE_ROUNDING=1 (the default since the clock-rate flip)
                         [--host-rounding]    # run the port with TE_EE_ROUNDING=0 (the pre-flip arithmetic: the RED reference)
    oracle_walk_judge.py --log RUN_LOG [--fixture ...]     # judge a --trace-player 1 run already recorded

The run: `--deterministic --no-sdl-input --no-audio --skip-movies --trace-state --trace-player 1 --play-pad <padlog> --stop-at
<stop_at>` with an empty `--mc-dir`, headless. The probe line per boundary (`[probe] @B pos (x y z) ... clock C frame_time T`)
is compared with the oracle's reference trace in the fixture (`oracle_trace`: boundary x y z per oracle frame, read by
tools/oracle/oracle_probe.py out of the oracle's memory captures; port boundary = oracle frame - `offset`).

Judged (the fixture freezes the numbers):
  * max |port position - oracle position| over the window's boundaries <= `max_position_delta` (units; Crash runs 0.15 a frame)
  * the game clock's step (`g_GlobalClock.time` between consecutive Playing boundaries of the window) == `clock_step` (149:
    retail's; the port's IEEE arithmetic gives 150, one frame in 150 ahead of retail and of the oracle -- the walk's drift;
    `frame_time` itself is the timer stamp difference and is 150 on both)
  * the run exits 0 at `--stop-at` with no wasm trap

Frozen on the TE_EE_ROUNDING knob (runner/main.cpp: the game thread's FPU rounding toward zero, the EE's fixed mode): RED on
the port as built before the clock-rate fix (clock step 150, the drift reaches 0.04 units by 2007), GREEN with the knob (clock step 149,
positions within 0.001 of the oracle's). Since the clock-rate flip the knob is the default: the judge is GREEN as built and
`--host-rounding` (TE_EE_ROUNDING=0) is the RED reference."""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_walk_forward_oracle.json")
PROBE = re.compile(r"\[probe\] @(\d+) pos \(([-\d.]+) ([-\d.]+) ([-\d.]+)\).*?gstate (\d+).*?clock (\d+) frame_time (\d+)")


def run(binary, iso, out, fx, ee_rounding, host_rounding=False):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    os.makedirs(mc)
    log = os.path.join(out, "run.log")
    cmd = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
           "--trace-player", "1", "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc]
    env = dict(os.environ, TE_GFX_HEADLESS="1")
    if ee_rounding:
        env["TE_EE_ROUNDING"] = "1"
    if host_rounding:
        env["TE_EE_ROUNDING"] = "0"
    with open(log, "w") as f:
        f.write("# %s%s%s\n" % ("TE_EE_ROUNDING=1 " if ee_rounding else "", "TE_EE_ROUNDING=0 " if host_rounding else "", " ".join(cmd)))
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env).returncode
    return log, rc


def read_trace(path):
    rows = {}
    for line in open(path):
        if line.startswith("#") or line.startswith("frame"):
            continue
        p = line.split()
        if len(p) >= 4:
            rows[int(p[0])] = (float(p[1]), float(p[2]), float(p[3]))
    return rows


def judge(log, fx, rc):
    oracle = read_trace(os.path.join(ROOT, fx["oracle_trace"]))
    offset = int(fx["offset"])
    lo, hi = fx["window"]
    port = {}
    trap = False
    for line in open(log):
        if "wasm trap" in line:
            trap = True
        m = PROBE.search(line)
        if m:
            port[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)))
    worst, worst_b, compared = 0.0, None, 0
    steps = {}
    for f, (ox, oy, oz) in sorted(oracle.items()):
        b = f - offset
        if b < lo or b > hi or b not in port:
            continue
        px, py, pz, gstate, clock, ft = port[b]
        d = max(abs(px - ox), abs(py - oy), abs(pz - oz))
        compared += 1
        if d > worst:
            worst, worst_b = d, b
        # the game clock's step this boundary (g_GlobalClock.time: TimeClockAdvanceBy's units; frameTime itself is the
        # timer stamp difference, 150 on both machines)
        if gstate == 12 and b - 1 in port and port[b - 1][3] == 12:
            step = clock - port[b - 1][4]
            steps[step] = steps.get(step, 0) + 1
    want_step = int(fx["clock_step"])
    bad_steps = sum(n for st, n in steps.items() if st != want_step)
    verdict = compared >= int(fx["min_compared"]) and worst <= float(fx["max_position_delta"]) and bad_steps == 0 and rc == 0 and not trap
    print("oracle_walk_judge: compared %d boundaries (%d..%d, oracle = port + %d); max |delta| %.3f at %s (max %.3f); clock step %s (want %d);"
          " exit %s%s" % (compared, lo, hi, offset, worst, worst_b, float(fx["max_position_delta"]),
                          ", ".join("%d x%d" % (k, v) for k, v in sorted(steps.items())) or "none", want_step,
                          rc if rc is not None else "n/a", ", TRAP" if trap else ""))
    print("VERDICT: %s" % ("GREEN" if verdict else "RED"))
    return 0 if verdict else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "judge_oracle_walk"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--log")
    ap.add_argument("--ee-rounding", action="store_true")
    ap.add_argument("--host-rounding", action="store_true")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.log:
        return judge(a.log, fx, 0)
    if not (a.bin and a.iso):
        ap.error("--bin and --iso (or --log)")
    log, rc = run(a.bin, a.iso, a.out, fx, a.ee_rounding, a.host_rounding)
    print("run: exit %d -> %s" % (rc, log))
    return judge(log, fx, rc)


if __name__ == "__main__":
    sys.exit(main())
