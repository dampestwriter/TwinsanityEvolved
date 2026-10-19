#!/usr/bin/env python3
"""RootMtvuStack -- the MTVU half of the composition root (INTEGRATION_PLAN rows
5.3 / 5.6; app/host/mtvu_stack.*) exists ONLY under engine.mtvu:

  --mode off   a 2-frame deterministic headless coldboot leg (the harness
               path: the profile pins engine.mtvu=false) reports `mtvu=off`,
               `savestate_refused_mtvu=0`, `vu1_library=title`, finalized=1.
  --mode play  a play-mode coldboot (`--harness-window --frames N`, N > 60,
               `--set engine.mtvu=on`) finishes its cap with
               `mtvu=on,worker=1,drain=<n>,drain_thread=inline|on,redirects=<n>,
               syncs=<12 ':' counts>,skipped=<12 ':' counts>,boundaries=<d>/<t>,
               declared=N,presented=N,batches=<b>,...` -- the worker ran
               (worker=1), every frame boundary went through the ladder
               (boundaries deferred + terminal == N, declared == presented == N)
               and VIF1 walks reached the worker (batches > 0); frames=N,
               finalized=1, video=gl. SKIP 77 without a display (video != gl).

Exit 77 (SKIP) when the title files are absent.
"""
import argparse
import os
import re
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--mode", choices=["off", "play"], required=True)
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection forwarded to the binary (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_mtvu_%s_" % args.mode)
    log = out_dir + ".log"
    if args.mode == "off":
        cmd = [args.runner, str(args.timeout), log, "--", args.binary,
               "--profile", "deterministic", "--headless", "--harness-out", out_dir,
               "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files)]
    else:
        cmd = [args.runner, str(args.timeout), log, "--", args.binary,
               "--harness-window", "--harness-out", out_dir, "--frames", str(args.frames),
               "--set", "engine.mtvu=on", "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "40"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "mtvu", "savestate_refused_mtvu", "vu1_library", "vu1_tier", "video",
                "dmac_tail_chases", "wall_ms"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    if args.mode == "off":
        check(proc.returncode == 0, "bounded run exits 0")
        check(meta.get("finalized") == "1", "finalized=1")
        check(meta.get("mtvu") == "off", "mtvu=off (the deterministic profile constructs no MTVU stack)")
        check(meta.get("savestate_refused_mtvu") == "0", "savestate_refused_mtvu=0 (no mtvu quiesce predicate)")
        check(meta.get("vu1_library") == "title", "vu1_library=title (hle_enabled = library != none)")
    else:
        # SKIP only for a leg that FINISHED without a GL window; a leg that never
        # finalized (crash/stall) is a failure, never a skip (review MAJOR 1).
        if meta.get("finalized") == "1" and meta.get("video") != "gl":
            print("SKIP: no window (video=%s): a play smoke needs a display (see %s)" % (meta.get("video", "<absent>"), log))
            shutil.rmtree(out_dir, ignore_errors=True)
            return 77
        check(proc.returncode == 0, "bounded run exits 0 (no stall, no crash)")
        check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)
        check(meta.get("finalized") == "1", "finalized=1")
        m = re.fullmatch(
            r"on,worker=(\d),drain=(\d+),drain_thread=(inline|on),redirects=(\d+),syncs=((?:\d+:){11}\d+),"
            r"skipped=((?:\d+:){11}\d+),boundaries=(\d+)/(\d+),declared=(\d+),presented=(\d+),batches=(\d+),.*",
            meta.get("mtvu", ""))
        check(m is not None, "mtvu=on,worker=...,syncs=<12>,skipped=<12>,boundaries=d/t,declared,presented,batches census shape")
        if m:
            check(m.group(1) == "1", "worker=1 (the MTVU worker thread ran to the cap)")
            deferred, terminal = int(m.group(7)), int(m.group(8))
            check(deferred + terminal == args.frames, "boundaries deferred+terminal == frames (%d+%d)" % (deferred, terminal))
            check(int(m.group(9)) == args.frames and int(m.group(10)) == args.frames,
                  "declared == presented == frames (runahead counters)")
            check(int(m.group(11)) > 0, "batches > 0 (VIF1 walks reached the worker through DmacMtvuFeed)")
        check(re.fullmatch(r"\d+", meta.get("savestate_refused_mtvu", "")) is not None, "savestate_refused_mtvu published")

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootMtvuStack[%s]: %s" % (args.mode, "FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
