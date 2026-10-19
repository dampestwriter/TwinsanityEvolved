#!/usr/bin/env python3
"""oracle_clock_judge.py -- the frozen judge of the ORACLE's game clock (the oracle half): over a Playing window the
recomp's game must advance its global clock by exactly 149 units a boundary. The oracle binary 2b14dc5b gives the game a
1174-unit frame TWICE every 512 boundaries: its timer 0 is tick-paced (9600 counts a boundary, no movement inside one), so
once per 512 boundaries the 16-bit count reads exactly 0 at every read of the boundary; the retail Ticks() (game
src/platform/ps2/time.cpp) takes a 0 with no overflow seen since its last read as an uncounted wrap, and -- the overflow
interrupt having already counted it and an earlier read of the same boundary having seen that -- adds 65536 once more; the
next frame's stamp is lower than the last, so GameTimeSetStamp keeps the long frame. Crash moves ~7 frames' worth in each of
the two frames (the leap in the walk pairs); every clock-driven system takes the same step.

    oracle_clock_judge.py [--oracle-bin BIN] [--oracle-iso NTSC.iso] [--out DIR] [--fixture fixtures/judges/hub_walk_forward_oracle_clock.json]

Two cold oracle legs on the fixture's oracle pad log (`--profile deterministic --headless --set harness.save_at_frame=N`), one
to each end of the window; tools/oracle/oracle_probe.py reads g_GlobalClock.time out of each state. Judged:
  * clock(end) - clock(start) == units_per_frame * (end - start)   (the fixture: 149 * (3320 - 2699) = 92529)
  * both legs exit 0 and both states exist

Frozen on 2b14dc5b: RED (the window holds two leaps: +4096 -- measured 96625). GREEN on an oracle whose timer 0 moves inside
the boundary (or whose game never reads the same count twice in one). The window must stay inside Playing with no load stall
(a stall is a legitimate long frame on both machines); the fixture's is the hub_walk_forward beach walk."""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_walk_forward_oracle_clock.json")
# the reference binary and the NTSC disc image: arguments, or these environment variables
DEFAULT_ORACLE = os.environ.get("TE_ORACLE_BIN", "")
DEFAULT_NTSC = os.environ.get("TE_NTSC_ISO", "")
PROBE = os.path.join(ROOT, "tools", "oracle", "oracle_probe.py")


def leg(oracle_bin, iso, padlog, out, n):
    d = os.path.join(out, "at_%d" % n)
    legdir, mc = os.path.join(d, "leg"), os.path.join(d, "mc")
    os.makedirs(legdir, exist_ok=True)
    os.makedirs(mc, exist_ok=True)
    cmd = [oracle_bin, "--profile", "deterministic", "--headless", "--frames", str(n + 3), "--inputs", padlog, "--iso", iso,
           "--set", "paths.mc_dir=" + mc, "--harness-out", legdir, "--set", "harness.save_at_frame=%d" % n, "--log-level", "warn"]
    with open(os.path.join(d, "run.log"), "w") as f:
        f.write("# " + " ".join(cmd) + "\n")
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    state = os.path.join(legdir, "savestates", "slot0.ps2st")
    if not os.path.exists(state):
        return rc, None, None
    line = subprocess.run([sys.executable, PROBE, state], capture_output=True, text=True).stdout
    os.remove(state)   # 77 MB; the probe line is what the judge keeps
    m = re.search(r"clock (\d+) frame_time (\S+)", line)
    return rc, int(m.group(1)) if m else None, line.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle-bin", default=DEFAULT_ORACLE)
    ap.add_argument("--oracle-iso", default=DEFAULT_NTSC)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "judge_oracle_clock"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    padlog = os.path.join(ROOT, fx["oracle_padlog"])
    start, end = int(fx["window"][0]), int(fx["window"][1])
    r0, c0, l0 = leg(a.oracle_bin, a.oracle_iso, padlog, a.out, start)
    r1, c1, l1 = leg(a.oracle_bin, a.oracle_iso, padlog, a.out, end)
    want = int(fx["units_per_frame"]) * (end - start)
    print("start @%d: %s" % (start, l0))
    print("end   @%d: %s" % (end, l1))
    ok = r0 == 0 and r1 == 0 and c0 is not None and c1 is not None
    got = (c1 - c0) if ok else None
    extra = (got - want) if ok else None
    print("oracle_clock_judge: clock advance over %d boundaries = %s, want %d (%d a frame)%s; exits %d / %d" % (
        end - start, got, want, int(fx["units_per_frame"]),
        "" if extra is None else " -- extra %d = %.1f leap frames of +%d" % (extra, extra / float(fx["leap_extra_units"]), int(fx["leap_extra_units"])),
        r0, r1))
    verdict = ok and got == want
    print("VERDICT: %s" % ("GREEN" if verdict else "RED"))
    return 0 if verdict else 1


if __name__ == "__main__":
    sys.exit(main())
