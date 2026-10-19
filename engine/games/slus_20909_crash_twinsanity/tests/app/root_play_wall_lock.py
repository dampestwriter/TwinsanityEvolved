#!/usr/bin/env python3
"""RootPlayWallLock -- the deferred tick and the pacer release at the root: a windowed, wall-paced coldboot
(`--frames N --harness-window`) under the wall-locked Play boundary. The pin
checks the STRUCTURE of the pacing, and MEASURES the pace (it does not require
the pace to be met: the gl frame can cost more than a field on this machine,
750 ms/frame measured; a late EE is reported through pace_late and never
burst-repaid).

run.meta checks:
  * frames=N, finalized=1                   -- the cap ended the leg
  * tick_thread=ticks=<k>,resyncs=<r>, k>0   -- the pacer ran
  * pace_parks + pace_late + pace_catchup == N -- EVERY boundary consumed the
                                               pacer's tick (parked for it, found
                                               it noted: late, or delivered a late
                                               frame's backlog field: catch-up --
                                               rule 3's single-field form)
  * pace_timeouts == 0                       -- the pacer never stopped under a
                                               parked boundary
  * ticks=...,deferred=<d> <= tick_thread k  -- no burst: the boundary never
                                               fired more ticks than the wall
                                               produced
  * pace_late_fields published; domain tick >= deferred - the clock contract
    (rule 3): every pacer tick the boundary fired is one elapsed field of the
    title's T0 (a late frame's fields are delivered, never absorbed), so the
    domain tick is never below the deferred ticks fired.
  * present_policy=live=a,hold=b,none=c,...  -- a + b + c == N (one decision per
                                               boundary), a > 0 (a first image)
  * wall_ms: printed; asserted within +-25 % of N * 16.667 ms ONLY when
    pace_late == 0 (the frame work fit the field); otherwise pace_late > 0 is
    reported as the measured fact (the leg ran late, boundaries == frames).

Exit 77 (SKIP) when the title files are absent, or when the run COMPLETED (runner
exit 0, finalized=1) and the root reported video=stub (no GL window). A crashed or
killed game process (runner exit != 0) or an unfinalized run.meta is a FAIL, never
a skip (cutover 2026-10-08). `--selftest` judges a fake runner (smoke_fake_runner.py).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
FIELD_MS = 1000.0 / 60.0


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


def completed_without_window(returncode, meta):
    """The only non-asset SKIP: the game process ran to its cap and finalized, but the root
    reported video=stub (a display-less host). A dead or unfinalized process never qualifies."""
    return returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub"


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    frames = ["--frames", "240"]
    cases = [
        ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames, 1, "FAIL bounded run exits 0"),
        ("crash exit 134, no run.meta", 134, None, frames, 1, "RootPlayWallLock: FAIL"),
        ("exit 0, finalized=0", 0, "frames=240\nfinalized=0\nvideo=gl\n", frames, 1, "FAIL finalized=1"),
        ("exit 0, finalized but stub video (display-less host)", 0, "frames=240\nfinalized=1\nvideo=stub\n", frames, 77, "SKIP: no window"),
        ("exit 134 with stub video is still a crash", 134, "frames=3\nfinalized=0\nvideo=stub\n", frames, 1, "RootPlayWallLock: FAIL"),
    ]
    ok = fake.expect(cases, script)
    print("root_play_wall_lock selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=240)
    ap.add_argument("--timeout", type=int, default=540)
    ap.add_argument("--selftest", action="store_true", help="judge a fake runner (no game process)")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection forwarded to the binary (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_play_wall_lock_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--harness-window", "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "90"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "ticks", "tick_thread", "pace_parks", "pace_late", "pace_catchup", "pace_timeouts", "pace_late_fields",
                "pace_dropped_fields", "pace_slept_ms",
                "domain", "present_policy", "video", "wall_ms", "fmv_active_frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if completed_without_window(proc.returncode, meta):
        print("SKIP: no window (video=stub, run completed and finalized): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    def as_int(s):
        return int(s) if s is not None and re.fullmatch(r"\d+", s) else -1

    n = args.frames
    check(proc.returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % proc.returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("video") == "gl", "video=gl (a window existed; video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("frames") == str(n), "frames=%d (the --frames cap ended the leg)" % n)
    m = re.fullmatch(r"ticks=(\d+),resyncs=(\d+)", meta.get("tick_thread", ""))
    check(m is not None and int(m.group(1)) > 0, "tick_thread=ticks=<k>,resyncs=<r> with k > 0 (the pacer ran)")
    parks, late, timeouts = as_int(meta.get("pace_parks")), as_int(meta.get("pace_late")), as_int(meta.get("pace_timeouts"))
    catchup = as_int(meta.get("pace_catchup", "0"))   # absent on an engine before the single-field rule 3: 0
    check(parks >= 0 and late >= 0 and catchup >= 0 and parks + late + catchup == n,
          "pace_parks + pace_late + pace_catchup == frames (%d + %d + %d == %d): every boundary consumed the pacer's tick" % (parks, late, catchup, n))
    check(timeouts == 0, "pace_timeouts == 0 (the pacer never stopped under a parked boundary)")
    pacer_ticks = int(m.group(1)) if m else -1
    td = re.search(r"deferred=(\d+)", meta.get("ticks", ""))
    deferred = int(td.group(1)) if td else -1
    check(deferred >= 0 and deferred <= pacer_ticks,
          "no burst: ticks deferred=%d <= tick_thread ticks=%d (the boundary never fired more ticks than the wall produced)"
          % (deferred, pacer_ticks))
    late_fields = as_int(meta.get("pace_late_fields"))
    dm = re.fullmatch(r"tick=(\d+),absorbed=(\d+),regime=(\d+)", meta.get("domain", ""))
    dom_tick, absorbed = (int(dm.group(1)), int(dm.group(2))) if dm else (-1, -1)
    check(dm is not None and late_fields >= 0, "domain=tick=<t>,absorbed=<a>,regime=<r> and pace_late_fields published")
    check(dom_tick >= deferred,
          "clock contract rule 3: domain tick %d >= deferred ticks %d (every pacer field fired reached T0; pace_late_fields=%d, absorbed=%d)"
          % (dom_tick, deferred, late_fields, absorbed))
    pp = re.fullmatch(r"live=(\d+),hold=(\d+),none=(\d+),cap=(\d+),cap_skips=(\d+),fmv=(\d+)", meta.get("present_policy", ""))
    if pp:
        live, hold, none = int(pp.group(1)), int(pp.group(2)), int(pp.group(3))
        check(live + hold + none == n, "present_policy decisions == frames (%d + %d + %d)" % (live, hold, none))
        check(live > 0, "present_policy: a first image was shown (live > 0)")
    else:
        check(False, "present_policy=live=<n>,hold=<n>,none=<n>,cap=<k>,cap_skips=<n>,fmv=<n> published")
    wall = as_int(meta.get("wall_ms"))
    expect = n * FIELD_MS
    print("  wall_ms=%d expected_at_pace=%.0f ratio=%.2f pace_parks=%d pace_late=%d" % (wall, expect, (wall / expect) if wall > 0 else -1, parks, late))
    if late == 0:
        check(wall > 0 and 0.75 * expect <= wall <= 1.25 * expect,
              "pace met: wall_ms within +-25%% of %d x %.2f ms" % (n, FIELD_MS))
    else:
        print("  NOTE pacing MEASURED, not met: pace_late=%d boundaries found the pacer's tick already noted (frame work > field)" % late)
        check(parks + late + catchup == n, "late + catch-up boundaries still == frames (no skipped field)")

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootPlayWallLock: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
