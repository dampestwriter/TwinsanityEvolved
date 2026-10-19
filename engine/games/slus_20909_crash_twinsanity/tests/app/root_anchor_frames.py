#!/usr/bin/env python3
"""root_anchor_frames -- an anchor leg runs to its --frames cap and finalizes (no park, no stall).

The shared body of RootAnchorF57292 / RootAnchorF59991: the built
`nativeee` is run as the harness arms an anchor leg (`--profile deterministic --headless
--harness-out <dir> --frames N --load-state <anchor> --game-files <dir> --set
video.backend=soft`), bounded through verify/run_bounded.sh with the harness's stall
detector on frames.fhash, and run.meta is asserted: finalized=1, frames=N, park=none,
restore_mhash_ok=1, and the bounded run exited 0. Before the BUG-072 fix the f57292 leg
stopped at 15 frames and the f59991 leg at ~126 (the walk FUN_0027edf0 spinning on a stack
frame below the scratchpad); the pin is the leg reaching N frames.

Exit 77 (SKIP) when the title files or the anchor are absent.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def run(name, argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--timeout", type=int, default=540)
    ap.add_argument("--keep", action="store_true", help="keep the harness-out dir and log even on success")
    args = ap.parse_args(argv)
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.anchor):
        print("SKIP: anchor absent: %s" % args.anchor)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    out_dir = tempfile.mkdtemp(prefix=name + "_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--load-state", os.path.abspath(args.anchor),
           "--game-files", os.path.abspath(args.game_files),
           "--set", "video.backend=soft",
           # the Soft backend hosts no selected Graphics producer: the pre-flip
           # control arm is stated explicitly (cutover flip 1 made sprites the default;
           # cutover flip 2 made the owned frame environment the default;
           # cutover flip 3 made native Strip primitives the default -- all three reverted)
           "--set", "hle.graphics_sprites=off",
           "--set", "hle.graphics_frame=off",
           "--set", "hle.graphics_strips=plain",
           # cutover flip 7 made the owned Movie pictures the default (a GL-only producer)
           "--set", "hle.graphics_movie=off",
           # ALL-ON build (2026-10-09): the Soft backend admits no Graphics producer and
           # the player chain requires graphics_movie -- the retained arms are stated
           # explicitly (default-independent; nothing weakened).
           "--set", "hle.graphics_text=off", "--set", "hle.graphics_sky=off",
           "--set", "hle.graphics_shadows=off", "--set", "hle.graphics_distortion=off",
           "--set", "hle.movie_step=off", "--set", "hle.movie_start=off",
           "--set", "hle.movie_wait=off", "--set", "hle.movie_stop=off"]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "120"   # a stalled walk is killed well inside the bound
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("finalized", "frames", "park", "park_pc", "park_detail", "restore_mhash_ok", "stop_reason"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    fhash = os.path.join(out_dir, "frames.fhash")
    n_lines = 0
    if os.path.isfile(fhash):
        with open(fhash, "rb") as f:
            n_lines = sum(1 for _ in f)
    print("frames.fhash lines: %d" % n_lines)
    check(proc.returncode == 0, "bounded run exits 0 (no stall kill, no bound)")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)
    check(meta.get("park", "none") in ("none", ""), "park=none")
    check(n_lines == args.frames, "frames.fhash carries %d lines" % args.frames)
    if failures == 0 and not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("%s: %s" % (name, "FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0
