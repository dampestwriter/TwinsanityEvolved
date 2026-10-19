#!/usr/bin/env python3
"""RootRestoresStreamHeartbeat -- after an anchor restore the STREAM.IRX heartbeat
hard timer is running in the live IOP kernel.

Runs the built `nativeee` binary as the harness does for an anchor leg
(`--profile deterministic --headless --harness-out <dir> --frames N
--state-every 1 --load-state <anchor> --game-files <dir>`), bounded through
verify/run_bounded.sh, and asserts on run.meta:

  * iop_timer_running  -- the timrman census at finalize lists hard timer 1 (the
                          module's 60 Hz heartbeat: its init RPC allocates, sets up
                          and starts it; the anchor's descriptor at IOP 0x14530
                          records it armed with compare 0x968c2); the beat's
                          handler is the iSignalSema that drives the streaming
                          worker's passes
  * iop_threads        -- the worker (tid 2) is registered and not Dormant at
                          finalize (the beat has a parked worker to signal)
  * iopm_restored=1, restore_mhash_ok=1, finalized=1, frames=N

A real process run, not a self-report: a restore that does not re-arm the timer
from the image's descriptor shows `iop_timer_running=none` (the loader restores
before the title's init frame, which is where OLD's loader had armed it). Exit 77
(SKIP) when the title files or the anchor are absent.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
HEARTBEAT_TIMER_ID = 1   # the module's only hard timer (AllocHardTimer -> 1 at its init RPC)
WORKER_TID = 2           # the module's streaming worker (entry 0x30d4, prio 40) in every anchor


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


def thread_states(census):
    """'1:Wait/Sleep,2:Wait/Sema' -> {1: 'Wait/Sleep', 2: 'Wait/Sema'}"""
    out = {}
    for item in census.split(","):
        if ":" not in item:
            continue
        tid, st = item.split(":", 1)
        try:
            out[int(tid)] = st
        except ValueError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.anchor):
        print("SKIP: anchor absent: %s" % args.anchor)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_stream_heartbeat_restore_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames), "--state-every", "1",
           "--load-state", os.path.abspath(args.anchor),
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
    for key in ("iopm_restored", "iop_timer_running", "iop_threads", "iop_cdvd", "cdvd_deliveries",
                "libsd_trans", "restore_mhash_ok", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("iopm_restored") == "1", "iopm_restored=1")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d" % args.frames)

    running = [t for t in meta.get("iop_timer_running", "").split(",") if t and t != "none"]
    check(str(HEARTBEAT_TIMER_ID) in running,
          "the heartbeat hard timer (id %d) is running after the restore (iop_timer_running lists it, not 'none')"
          % HEARTBEAT_TIMER_ID)
    final = thread_states(meta.get("iop_threads", ""))
    check(WORKER_TID in final and not final[WORKER_TID].startswith("Dormant"),
          "at finalize the worker (tid %d) the beat signals is registered and not Dormant" % WORKER_TID)

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootRestoresStreamHeartbeat: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
