#!/usr/bin/env python3
"""RootHoldsFmvAbort -- the composition root installs the title's FmvAbortHold
(INTEGRATION_PLAN row 7.7, the plan-sanctioned emulation-bug workaround) and the
coldboot leg then serves the golden picture windows of both intro movies.

Runs the built `nativeee` binary exactly as the harness does (a deterministic
headless coldboot leg), bounded through verify/run_bounded.sh, and asserts on the
produced run.meta:

  * fmv_served_per_playback starts with 201,346  -- the golden windows (P7-GOLDEN-WINDOW
                                                   s1=201, s2=346) under the live binary
  * fmv_pictures_served=547                      -- the two windows, nothing else
  * fmv_abort_holds is a count >= 1 without `,nohook` -- the hold is installed and fired
                                                   (the title latches the abort at the
                                                   instant end of file once per movie)
  * finalized=1, frames=N                        -- the --frames cap ended the leg

Without the hold the same leg reports fmv_served_per_playback=2,2+ (the two priming
serves per movie; INT-I73B-FMV-TWO-PICTURES). Exit 77 (SKIP) when the title files
are absent.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection (e.g. hle.movie_lifecycle=off: the retained arm this pin measures)")
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_fmv_abort_hold_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0
    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("fmv", "fmv_served_per_playback", "fmv_pictures_served", "fmv_abort_holds",
                "park", "park_detail", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    served = meta.get("fmv_served_per_playback", "")
    check(served.startswith("201,346"), "fmv_served_per_playback starts with 201,346 (the golden windows)")
    check(meta.get("fmv_pictures_served") == "547", "fmv_pictures_served=547")
    holds = meta.get("fmv_abort_holds", "")
    check("nohook" not in holds, "fmv_abort_holds carries no `,nohook` (FmvAbortHold installed at the scene updater)")
    check(holds.split(",")[0].isdigit() and int(holds.split(",")[0]) >= 1,
          "fmv_abort_holds >= 1 (the premature abort was held at least once)")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootHoldsFmvAbort: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
