#!/usr/bin/env python3
"""RootRegistersVuVifGifSections -- the composition root registers the VU
memory, VU1 register, VIFS and GIFS savestate sections (and TAG_DMAC).

Runs the built `nativeee` binary exactly as the harness does for an anchor leg
(verify/run_leg.py: `--profile deterministic --headless --harness-out <dir>
--frames N --load-state <anchor> --game-files <dir>`), bounded through
verify/run_bounded.sh, and asserts on the produced run.meta:

  * vu_sections_restored=4/1/1       -- the four VU memories restored, VU1R
                                        restored once, its running flag seen once
                                        (every frozen anchor carries running = 1)
  * vif_gif_sections_restored=1/1/0  -- VIFS once, GIFS once, no parser tail bytes
  * dmac_save_unquiesced=0           -- the DMAC section is registered and the
                                        capture quiesce found nothing running
  * restore_mhash_ok=1, finalized=1, frames=N

A real process run, not a self-report: a root that drops one of the Register
calls leaves the counters at 0 and fails this test. Exit 77 (SKIP) when the
title files or the anchor are absent.
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

    out_dir = tempfile.mkdtemp(prefix="root_vu_vif_gif_sections_")
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
    for key in ("vu_sections_restored", "vif_gif_sections_restored", "dmac_save_unquiesced",
                "restore_mhash_ok", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("vu_sections_restored") == "4/1/1",
          "vu_sections_restored=4/1/1 (VuMemorySection x4 + Vu1RegisterSection registered; anchor running flag seen)")
    check(meta.get("vif_gif_sections_restored") == "1/1/0",
          "vif_gif_sections_restored=1/1/0 (VifsSection + GifsSection registered; empty parser tail)")
    check(meta.get("dmac_save_unquiesced") == "0", "dmac_save_unquiesced=0 (TAG_DMAC registered; capture quiesced)")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
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
    print("RootRegistersVuVifGifSections: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
