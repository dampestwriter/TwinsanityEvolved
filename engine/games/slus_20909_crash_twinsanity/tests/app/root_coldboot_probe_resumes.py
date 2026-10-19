#!/usr/bin/env python3
"""RootColdbootProbeResumes: a cold-boot boundary image written by the
original tree (the coldboot leg's frozen probe `old_b120.ps2st`, boundary 120) restores on
this runtime AND RESUMES on the same timeline.

The leg: `--load-state <probe> --frames 61` (deterministic profile, soft backend). The pins:
  * `finalized=1` and `frames=61` in run.meta -- the resumed title raises boundaries at all
    (before the fix no boundary followed the restore: the HEAP section's META-arena
    bookkeeping was never restored, the first routed metadata allocation re-vended the
    title's live compaction job, the compactor copied through garbage descriptors and the
    allocator's bin walk spun on a NULL link);
  * `heap_section_restored=1` and `restore_mhash` = the lane's `mem.mhash` line at the probe
    boundary (the restore is byte-exact);
  * the IDENTITY pin: the resumed leg's FIRST presented frame (frames.fhash key 1) hashes
    equal to the frozen cold-boot lane's frame at boundary <probe + 1> -- the restored GS
    state and the title's first resumed frame reproduce the original tree's own next frame
    (the gl backend: the soft backend presents the black frame only). A RAM0 identity
    against the original lane 60 boundaries later is NOT expected: coldboot is a divergent
    leg (the attribution ledger's classes), so the lane's later mem lines are the original
    tree's, not this runtime's continuation.
The probe boundary is read from the probe's file name (`old_b<N>.ps2st`). SKIP 77 when the
title files, the probe or the lane files are absent.
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
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def read_lane(path):
    """key -> hash string of a `<key>,<hash>` lane file."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or "," not in line:
                continue
            k, h = line.split(",", 1)
            out[int(k)] = h.strip()
    return out


def probe_boundary(path):
    m = re.search(r"old_b(\d+)\.ps2st$", os.path.basename(path))
    return int(m.group(1)) if m else None


def judge(meta, leg_frames, lane_mem, lane_frames, boundary, frames):
    """The pins over parsed inputs (pure; selftest)."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta.get("finalized") == "1", "finalized=1 (the leg reached its frame cap)")
    check(meta.get("frames") == str(frames), "frames=%d (run.meta frames=%s)" % (frames, meta.get("frames")))
    check(meta.get("heap_section_restored") == "1",
          "heap_section_restored=1 (run.meta %s)" % meta.get("heap_section_restored", "<absent>"))
    restore = meta.get("restore_mhash", "")
    lane_at_probe = lane_mem.get(boundary)
    check(lane_at_probe is not None and restore.lower() == lane_at_probe.lower(),
          "restore_mhash %s == lane mem line %d %s" % (restore, boundary, lane_at_probe))
    first = leg_frames.get(1)
    lane_next = lane_frames.get(boundary + 1)
    check(first is not None and lane_next is not None and first.lower() == lane_next.lower(),
          "identity: resumed first frame (key 1) %s == cold-boot lane frame %d %s" % (first, boundary + 1, lane_next))
    return results


def selftest():
    ok = True
    lane = {120: "0x74bacf01b589b5a6", 180: "0xde50cbd1117ac503"}
    lane_frames = {120: "0x91112b3860dce5fc", 121: "0x91112b3860dce5fc"}
    good_meta = {"finalized": "1", "frames": "61", "heap_section_restored": "1", "restore_mhash": "0x74bacf01b589b5a6"}
    good_leg = {1: "0x91112b3860dce5fc", 122: "0x91112b3860dce5fc"}
    r = judge(good_meta, good_leg, lane, lane_frames, 120, 61)
    ok &= all(x for x, _ in r)
    # the pre-fix shape: no boundary, no HEAP restore, no frame
    bad_meta = {"finalized": "0", "frames": "0", "restore_mhash": "0x74bacf01b589b5a6"}
    r = judge(bad_meta, {}, lane, lane_frames, 120, 61)
    fails = [w for x, w in r if not x]
    ok &= len(fails) == 4 and any("identity" in w for w in fails) and any("heap_section" in w for w in fails)
    # a resumed leg presenting ANOTHER frame: only the identity pin fails
    r = judge(good_meta, {1: "0x0000000000000001"}, lane, lane_frames, 120, 61)
    fails = [w for x, w in r if not x]
    ok &= fails == [w for w in fails if "identity" in w] and len(fails) == 1
    ok &= probe_boundary("/x/old_b120.ps2st") == 120 and probe_boundary("/x/state_b5.ps2st") is None
    print("root_coldboot_probe_resumes selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--probe", help="the frozen cold-boot boundary image old_b<N>.ps2st")
    ap.add_argument("--lane", help="the cold-boot leg's frozen lane dir (mem.mhash + frames.fhash)")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=61)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "probe", "lane", "runner"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    lane_mem_path = os.path.join(args.lane, "mem.mhash")
    lane_frames_path = os.path.join(args.lane, "frames.fhash")
    if not os.path.isfile(args.probe) or not os.path.isfile(lane_mem_path) or not os.path.isfile(lane_frames_path):
        print("SKIP: probe or lane absent: %s / %s" % (args.probe, args.lane))
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    boundary = probe_boundary(args.probe)
    if boundary is None:
        print("FAIL: probe name carries no boundary: %s" % args.probe)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_coldboot_probe_resumes_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--load-state", os.path.abspath(args.probe),
           "--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(min(args.timeout, 90))
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    frames_path = os.path.join(out_dir, "frames.fhash")
    leg_frames = read_lane(frames_path) if os.path.isfile(frames_path) else {}
    lane_mem = read_lane(lane_mem_path)
    lane_frames = read_lane(lane_frames_path)
    for key in ("frames", "finalized", "wall_ms", "restore_mhash", "heap_section_restored",
                "meta_arena_bump_restored", "restore_thrd_records", "relaunch", "park", "park_detail"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    failures = 0
    for ok, what in judge(meta, leg_frames, lane_mem, lane_frames, boundary, args.frames):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootColdbootProbeResumes: FAIL (%d) -- leg kept at %s, log %s" % (failures, out_dir, log))
        return 1
    shutil.rmtree(out_dir, ignore_errors=True)
    if os.path.isfile(log):
        os.remove(log)
    print("RootColdbootProbeResumes: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
