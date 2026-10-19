#!/usr/bin/env python3
"""RootBindsDmacSyncSeams -- the composition root binds the DMAC's sce-surface
observation points (INTEGRATION_PLAN item 5.5): GsSync::BindDmac (the frame
sync's ChaseVif1Tail before the boundary sequence, sceGsSyncPath's flush) and
the six libdma.a manifest rows (the sceDmaSync / sceGifSyncDma seams included).

Runs the built `nativeee` binary exactly as the harness does for an anchor leg
(verify/run_leg.py: `--profile deterministic --headless --harness-out <dir>
--frames N --load-state <anchor> --game-files <dir>`), bounded through
verify/run_bounded.sh, and asserts on the produced run.meta:

  * dmac_tail_chases=N        -- one chase per boundary (every sceGsSyncV of the
                                 N-frame leg ran the bound Dmac::ChaseVif1Tail)
  * gs_syncpath_flushes >= 1  -- the bound flush ran at sceGsSyncPath (the title
                                 calls it every frame at this anchor)
  * libdma_rows=6             -- LibdmaBindings::Bind registered the sync rows
  * libdma_calls has sync=/gifsync= fields; dmac_tail_chases_parked and
    gs_syncpath_parked present (the "found a parked kick" census)
  * finalized=1, frames=N

A real process run, not a self-report: a root that drops the BindDmac call
leaves dmac_tail_chases at 0 and fails this test. Exit 77 (SKIP) when the title
files or the anchor are absent.
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
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
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

    out_dir = tempfile.mkdtemp(prefix="root_dmac_sync_seams_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
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
    for key in ("dmac_tail_chases", "dmac_tail_chases_parked", "gs_syncpath_flushes", "gs_syncpath_parked",
                "libdma_rows", "libdma_calls", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    def as_int(key):
        try:
            return int(meta.get(key, ""))
        except ValueError:
            return -1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("dmac_tail_chases") == str(args.frames),
          "dmac_tail_chases=%d (GsSync::BindDmac bound: one ChaseVif1Tail per boundary)" % args.frames)
    check(as_int("gs_syncpath_flushes") >= 1, "gs_syncpath_flushes >= 1 (sceGsSyncPath runs the bound flush)")
    check(as_int("dmac_tail_chases_parked") >= 0 and as_int("gs_syncpath_parked") >= 0,
          "parked-kick census fields present")
    check(meta.get("libdma_rows") == "6", "libdma_rows=6 (the sceDmaSync / sceGifSyncDma rows registered)")
    calls = meta.get("libdma_calls", "")
    check(",sync=" in calls and ",gifsync=" in calls, "libdma_calls carries sync=/gifsync=")
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
    print("RootBindsDmacSyncSeams: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
