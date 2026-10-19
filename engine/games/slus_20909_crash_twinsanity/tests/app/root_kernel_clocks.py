#!/usr/bin/env python3
"""RootInstallsKernelClocks -- the composition root installs the kernel clocks.

Runs the built `nativeee` binary exactly as the harness does (verify/run_leg.py:
`--profile deterministic --headless --harness-out <dir> --frames N --game-files
<dir>`), bounded through verify/run_bounded.sh, and asserts on the produced
run.meta:

  * kernl_timer_clock=1  -- KernlTimer has a clock (DerivedClocks::InstallKernelClocks
                            ran in the root; the game's TimerWaitMicroseconds counts
                            T0 in the tick domain instead of parking)
  * finalized=1          -- the leg closed normally
  * park is not tick_clock_absent

A real process run, not a self-report: a future root that drops the install line
fails this test. Exit 77 (SKIP) when the title files are absent.
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
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_kernel_clocks_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
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
    for key in ("kernl_timer_clock", "finalized", "park", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("kernl_timer_clock") == "1",
          "kernl_timer_clock=1 (the root installed DerivedClocks::InstallKernelClocks)")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("park") != "tick_clock_absent", "park is not tick_clock_absent")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootInstallsKernelClocks: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
