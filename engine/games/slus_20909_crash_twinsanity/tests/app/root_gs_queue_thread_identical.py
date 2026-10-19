#!/usr/bin/env python3
"""RootGsQueueThreadIdentical / RootGsQueuePipelinedIdentical -- the gs.queue byte-identity
pin (plan row 6.3).

With gs.queue=thread the DrawIR stream is carried to the DrawPipeline on its own
consumer thread (the GL context lives there) in lockstep with the EE's pushes; with
gs.queue=pipelined the pushes return at once and the pipeline runs on its own VRAM
image fed by the EE's logged calls in stream order. In either mode
the deterministic lanes must equal gs.queue=inline's. A deterministic headless coldboot
leg and the artdet anchor leg are run through verify/run_leg.py --tree new in BOTH
modes on the same binary and compared lane by lane with a byte compare: frames.fhash,
mem.mhash, drawir.ohash, gsops.ohash (whole files) and audio.ahash over the first N-1
windows (the last window is the partial tail of the run). Prints `pin=IDENTICAL` or
`pin=DIVERGENT <leg>/<lane> line L`. Also prints each leg's wall_ms and the thread
mode's gs_queue= census (measurement, not asserted).

`--inputs <session.padlog> --inputs-frames N` adds a third leg: the deterministic
replay of a recorded play session (the all-on composition's route through the
title, the FMV and the level) over N frames, in both modes.

Exit 77 (SKIP) when the title files or the anchor are missing; 0 identical;
1 divergent / a leg failed.

  root_gs_queue_thread_identical.py --binary <bin> --anchor <artdet.ps2st>
        [--repo <OLD root>] [--out <dir>] [--coldboot-frames 130] [--anchor-frames 60]
        [--inputs <padlog> --inputs-frames 4000]
        [--timeout 600] [--modes inline,thread] [--keep]
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


def meta_value(leg_dir, key):
    lines = read_lines(os.path.join(leg_dir, "run.meta")) or []
    for ln in lines:
        if ln.startswith(key + "="):
            return ln[len(key) + 1:]
    return "?"


def run_leg(binary, anchor, frames, out_dir, repo, timeout, mode, settings=(), inputs=None):
    cmd = [sys.executable, RUN_LEG, "--binary", binary, "--anchor", anchor, "--frames", str(frames),
           "--out", out_dir, "--tree", "new", "--repo", repo, "--timeout", str(timeout),
           "--set", "gs.queue=" + mode]
    if inputs:
        # A recorded play session replayed from a cold boot, fresh memory card.
        mc_dir = os.path.join(os.path.dirname(out_dir), os.path.basename(out_dir) + "_mc")
        shutil.rmtree(mc_dir, ignore_errors=True)
        os.makedirs(mc_dir)
        cmd += ["--inputs", os.path.abspath(inputs), "--set", "paths.mc_dir=" + mc_dir]
    for setting in settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    sys.stdout.flush()
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
    print("  -> exit=%d %s" % (proc.returncode, tail))
    return proc.returncode == 0


def compare_lane(a, b, names, drop_last=False):
    """None when identical, else 'line L: <a> vs <b>' (1-based; <absent> for a missing line)."""
    if a is None or b is None:
        if a is None and b is None:
            return "file missing on both"
        return "file missing on %s" % (names[0] if a is None else names[1])
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


def pin(binary, repo, out, legs, timeout, modes, settings=()):
    """Returns (verdict_line, details[]) over legs [(name, anchor_or_coldboot, frames[, padlog])]."""
    details = []
    first_div = None
    base = modes[0]
    for leg in legs:
        name, anc, frames = leg[0], leg[1], leg[2]
        inputs = leg[3] if len(leg) > 3 else None
        dirs = {}
        for mode in modes:
            d = os.path.join(out, "%s_%s" % (name, mode))
            if not run_leg(binary, anc, frames, d, repo, timeout, mode, settings, inputs):
                return "pin=DIVERGENT %s/%s leg failed" % (name, mode), details
            dirs[mode] = d
            details.append("%s/%s: wall_ms=%s frames=%s gs_queue=%s" % (
                name, mode, meta_value(d, "wall_ms"), meta_value(d, "frames"), meta_value(d, "gs_queue")))
        for mode in modes[1:]:
            for lane in WHOLE_LANES + (AUDIO_LANE,):
                la = read_lines(os.path.join(dirs[base], lane))
                lb = read_lines(os.path.join(dirs[mode], lane))
                why = compare_lane(la, lb, (base, mode), drop_last=(lane == AUDIO_LANE))
                n = min(len(la or []), len(lb or []))
                shown = (n - 1) if lane == AUDIO_LANE else n
                if why is None:
                    details.append("%s/%s %s==%s: identical (%d lines%s)" % (
                        name, lane, base, mode, shown, ", first N-1 windows" if lane == AUDIO_LANE else ""))
                else:
                    details.append("%s/%s %s vs %s: DIVERGENT %s" % (name, lane, base, mode, why))
                    if first_div is None:
                        first_div = "%s/%s %s" % (name, lane, why.split(":")[0])
    if first_div:
        return "pin=DIVERGENT " + first_div, details
    return "pin=IDENTICAL", details


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--anchor", required=True, help="the artdet anchor .ps2st")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--out")
    ap.add_argument("--coldboot-frames", type=int, default=130)
    ap.add_argument("--anchor-frames", type=int, default=60)
    ap.add_argument("--inputs", help="a recorded play session (.padlog) replayed deterministically as a third leg")
    ap.add_argument("--inputs-frames", type=int, default=4000)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--modes", default="inline,thread", help="comma list; the first is the reference")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--set", action="append", default=[], dest="settings", help="explicit Config selection forwarded to both legs (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.repo, "game_files", ELF_REL)):
        print("SKIP: title files absent under %s" % os.path.join(a.repo, "game_files"))
        return 77
    if not os.path.isfile(a.binary):
        print("FAIL: binary missing: %s" % a.binary)
        return 1
    if a.anchor_frames > 0 and not os.path.isfile(a.anchor):
        print("SKIP: anchor absent: %s" % a.anchor)
        return 77
    modes = [m.strip() for m in a.modes.split(",") if m.strip()]
    if len(modes) < 2:
        print("FAIL: --modes needs at least two modes")
        return 1
    out = a.out or tempfile.mkdtemp(prefix="root_gs_queue_thread_identical_")
    os.makedirs(out, exist_ok=True)
    legs = []
    if a.coldboot_frames > 0:
        legs.append(("coldboot", "coldboot", a.coldboot_frames))
    if a.anchor_frames > 0:
        legs.append(("artdet", os.path.abspath(a.anchor), a.anchor_frames))
    if a.inputs:
        if not os.path.isfile(a.inputs):
            print("SKIP: padlog absent: %s" % a.inputs)
            return 77
        legs.append(("session", "coldboot", a.inputs_frames, a.inputs))
    verdict, details = pin(a.binary, a.repo, out, legs, a.timeout, modes, a.settings)
    for d in details:
        print("  " + d)
    print(verdict)
    ok = verdict == "pin=IDENTICAL"
    if ok and not a.keep:
        shutil.rmtree(out, ignore_errors=True)
    else:
        print("artifacts kept: %s" % out)
    print("RootGsQueue%sIdentical: %s" % ("Pipelined" if "pipelined" in modes else "Thread", "ALL PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
