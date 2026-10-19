#!/usr/bin/env python3
"""RootHeadroomQueryRecompiled -- the composition root puts the title's RECOMPILED
headroom predicate back at 0x191bd0 (HeadroomQueryGlue) before
LoaderSpinLift wraps the address, so the loader's headroom queries fire NO Demand
tick and T0 advances per boundary only.

Runs the built `nativeee` binary the way the harness does (a deterministic
headless coldboot leg of N frames; N >= 130 covers the whole loader phase) and
asserts on run.meta:

  * headroom_query=recompiled      -- the glue's install answered true and the
                                      loader_sync Layout agrees on the address
  * ticks=...demand=N...           -- every Demand tick is a wait's (FMV park_ticks + disc parks); none is the query's (the
                                      emitted hook fired one per query: 638 on the
                                      130-frame leg before the re-binding)
  * ticks=boundary=N               -- the boundary cause still fires once per frame
  * loader_spin_lifts=lap1=..,lap2=..,declines=..,idle=..,iop_pumps=..,rpc=..,edge=..  present, no `,nohook` (the lift wrapped the
                                      predicate, not the wait)
  * finalized=1, frames=N

A real process run, not a self-report. Exit 77 (SKIP) when the title files are
absent.
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


def census(field):
    """'a=1,b=2' -> {'a': '1', 'b': '2'}"""
    out = {}
    for part in field.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection (e.g. hle.stream_files=off: the retained arm this pin measures)")
    ap.add_argument("--frames", type=int, default=130)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_headroom_query_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
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
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("frames", "finalized", "headroom_query", "ticks", "loader_spin_lifts", "park", "wall_ms"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d" % args.frames)
    check(meta.get("headroom_query") == "recompiled",
          "headroom_query=recompiled (HeadroomQueryGlue installed the generated predicate over the wait hook)")
    ticks = census(meta.get("ticks", ""))
    # Every Demand tick is a wait's: the FMV pacer's parks (fmv=...park_ticks=N) and the
    # disc wait parks under cdvd.delivery_visibility=poll (cdvd_wait_parks=<frames>/<waits>,
    # one Demand tick per parked frame). The headroom query itself fires none (was 638
    # under the wait seam): demand == park_ticks + cdvd parked frames.
    fmv = dict(kv.split("=", 1) for kv in meta.get("fmv", "").split(",") if "=" in kv)
    fmv_parks = int(fmv.get("park_ticks", "0") or 0)
    cdvd_parks = int((meta.get("cdvd_wait_parks", "0/0").split("/") + ["0"])[0] or 0)
    demand = int(ticks.get("demand", "-1"))
    check(demand == fmv_parks + cdvd_parks,
          "ticks demand=%d == fmv park_ticks %d + cdvd parked frames %d (the headroom query fires no Demand tick)" % (demand, fmv_parks, cdvd_parks))
    check(ticks.get("boundary") == str(args.frames), "ticks boundary=%d (one boundary tick per frame)" % args.frames)
    lifts = meta.get("loader_spin_lifts", "")
    check(re.fullmatch(r"lap1=\d+,lap2=\d+,declines=\d+,idle=\d+,iop_pumps=\d+,rpc=\d+,edge=\d+", lifts) is not None,
          "loader_spin_lifts=lap1=..,lap2=..,declines=..,idle=..,iop_pumps=..,rpc=..,edge=.. without ,nohook (the lift wraps the predicate)")

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootHeadroomQueryRecompiled: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
