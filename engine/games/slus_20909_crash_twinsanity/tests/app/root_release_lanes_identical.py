#!/usr/bin/env python3
"""RootReleaseLanesIdentical -- the Release-vs-Debug identity guard.

The batteries run on the Release binary (12 legs x 3 reps in ~30 min instead of
~4 h); the guard proves, per run, that the Release build's deterministic lanes equal
the Debug build's (the ctest/pin binary): a deterministic headless coldboot leg
`--frames 200` and the artdet anchor leg `--frames 60` are run on BOTH binaries
through verify/run_leg.py --tree new and compared lane by lane with a byte compare:
frames.fhash, mem.mhash, drawir.ohash, gsops.ohash (whole files) and audio.ahash
over the first N-1 windows (the last window is the partial tail of the run and is
written but not claimed). Prints `guard=IDENTICAL` or `guard=DIVERGENT <leg>/<lane>
line L` naming the first differing lane and line.

Exit 77 (SKIP) when the Release binary is absent (no build-release/ configured) or
the title files are missing; 0 identical; 1 divergent / a leg failed.

  root_release_lanes_identical.py --debug <bin> --release <bin> --anchor <artdet.ps2st>
        [--repo <OLD root>] [--out <dir>] [--coldboot-frames 200] [--anchor-frames 60]
        [--timeout 600] [--set key=val ...] [--keep]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
RUN_LEG = os.path.join(REPO_ROOT, "verify", "run_leg.py")
DEFAULT_REPO = os.environ.get("PS2R_OLD_ROOT", "")  # the frozen OLD tree (read-only); empty = absent
ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
WHOLE_LANES = ("frames.fhash", "mem.mhash", "drawir.ohash", "gsops.ohash")
AUDIO_LANE = "audio.ahash"


def read_lines(path):
    if not os.path.isfile(path):
        return None
    with open(path, errors="replace") as f:
        return [ln.rstrip("\n") for ln in f if ln.strip()]


def run_leg(binary, anchor, frames, out_dir, repo, timeout, sets):
    cmd = [sys.executable, RUN_LEG, "--binary", binary, "--anchor", anchor, "--frames", str(frames),
           "--out", out_dir, "--tree", "new", "--repo", repo, "--timeout", str(timeout)]
    for kv in sets or ():
        cmd += ["--set", kv]
    print("run: " + " ".join(cmd))
    sys.stdout.flush()
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    print("  -> exit=%d %s" % (proc.returncode, tail))
    return proc.returncode == 0


def compare_lane(name, a, b, drop_last=False):
    """None when identical, else 'line L: <a> vs <b>' (1-based; <absent> for a missing line)."""
    if a is None or b is None:
        return "file %s" % ("missing on both" if a is None and b is None else
                            ("missing on debug" if a is None else "missing on release"))
    if drop_last:
        n = min(len(a), len(b)) - 1
        a, b = a[:n], b[:n]
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return "line %d: %s vs %s" % (i + 1, a[i], b[i])
    if len(a) != len(b):
        return "line %d: %s vs %s" % (n + 1, a[n] if n < len(a) else "<absent>",
                                      b[n] if n < len(b) else "<absent>")
    return None


def guard(debug, release, anchor, repo, out, legs, timeout, sets):
    """Returns (verdict_line, details[]) -- the guard over the given legs
    [(name, anchor_or_coldboot, frames)]."""
    details = []
    first_div = None
    for name, anc, frames in legs:
        dirs = {}
        for side, binary in (("debug", debug), ("release", release)):
            d = os.path.join(out, "%s_%s" % (name, side))
            if not run_leg(binary, anc, frames, d, repo, timeout, sets):
                return "guard=DIVERGENT %s/%s leg failed" % (name, side), details
            dirs[side] = d
        for lane in WHOLE_LANES + (AUDIO_LANE,):
            la = read_lines(os.path.join(dirs["debug"], lane))
            lb = read_lines(os.path.join(dirs["release"], lane))
            why = compare_lane(lane, la, lb, drop_last=(lane == AUDIO_LANE))
            n = min(len(la or []), len(lb or []))
            shown = (n - 1) if lane == AUDIO_LANE else n
            if why is None:
                details.append("%s/%s: identical (%d lines%s)" % (
                    name, lane, shown, ", first N-1 windows" if lane == AUDIO_LANE else ""))
            else:
                details.append("%s/%s: DIVERGENT %s" % (name, lane, why))
                if first_div is None:
                    first_div = "%s/%s %s" % (name, lane, why.split(":")[0])
    if first_div:
        return "guard=DIVERGENT " + first_div, details
    return "guard=IDENTICAL", details


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", required=True)
    ap.add_argument("--release", required=True)
    ap.add_argument("--anchor", required=True, help="the artdet anchor .ps2st")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--out")
    ap.add_argument("--coldboot-frames", type=int, default=200)
    ap.add_argument("--anchor-frames", type=int, default=60)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--set", action="append", metavar="KEY=VAL")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    if not os.path.isfile(a.release):
        print("SKIP: Release binary absent: %s" % a.release)
        return 77
    if os.path.isfile(a.debug) and os.path.samefile(a.debug, a.release):
        print("FAIL: Debug and Release must be distinct build executables")
        return 1
    if not os.path.isfile(os.path.join(a.repo, "game_files", ELF_REL)):
        print("SKIP: title files absent under %s" % os.path.join(a.repo, "game_files"))
        return 77
    if not os.path.isfile(a.debug):
        print("FAIL: Debug binary missing: %s" % a.debug)
        return 1
    if not os.path.isfile(a.anchor):
        print("SKIP: anchor absent: %s" % a.anchor)
        return 77
    out = a.out or tempfile.mkdtemp(prefix="root_release_lanes_identical_")
    os.makedirs(out, exist_ok=True)
    legs = [("coldboot", "coldboot", a.coldboot_frames), ("artdet", os.path.abspath(a.anchor), a.anchor_frames)]
    verdict, details = guard(a.debug, a.release, a.anchor, a.repo, out, legs, a.timeout, a.set)
    for d in details:
        print("  " + d)
    print(verdict)
    ok = verdict == "guard=IDENTICAL"
    if ok and not a.keep:
        shutil.rmtree(out, ignore_errors=True)
    else:
        print("artifacts kept: %s" % out)
    print("RootReleaseLanesIdentical: %s" % ("ALL PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
