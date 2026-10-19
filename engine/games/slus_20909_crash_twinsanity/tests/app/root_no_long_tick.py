#!/usr/bin/env python3
"""RootNoLongTick (FRAME SKIP): no game tick exceeds the field period
outside a declared load or FMV window, on a play-profile wall-paced replay of a session.

The clock contract lets a tick that cannot finish in one field deliver the
elapsed fields -- a larger step, never slow motion -- but the step is VISIBLE (s8:
`pace_late_trace 1178:44984:2`, one 45 ms tick at the level-entry cut, a frame dropped).
The standing rule: no single tick exceeds the period outside a declared load.

What the leg publishes (run.meta; BoundaryProtocol + TickProfile):
  pace_long=<n>                      every boundary whose frame work exceeded one period
  pace_long_trace=<ordinal>:<work_us>:<pending>:<site breakdown>;...  (the first 160)
  load_windows=<first>-<last>,...    the loader census in boundary ordinals (a tick whose
                                     loader site read a file window or waited a completion
                                     is a load tick; load ticks within kLoadGapTicks form
                                     one window)
  load_tail_ticks=<n>                published; the judge applies NO tail (0): the windows are
                                     the title's own load phase -- the loader's file windows AND
                                     the title's chunk state machine (ChunkStateCensus: loading /
                                     unloading / releasing chunk steps and the CUT, a chunk's
                                     first Shown update = the level's first update, every agent's
                                     state applied / script run / ground raycast). The tick after
                                     a window is a PLAY tick and is judged (the s9 cut 1178 fell in
                                     baf15a38's 15-tick tail: the red-before).
  chunk_cuts=<n>:<ord>,...           the cut ticks (required; absent on baf15a38)
  fmv_window=<first>-<last>|none     the movie holds the tick by design (declared)

Judged: every long tick's ordinal lies inside a load window or the FMV window (-1) -- no
tail; every cut tick lies inside a load window; the rows exist (the frozen all-on binary
91dfdbe6 publishes none of them; baf15a38 publishes no chunk_cuts and a 15-tick tail: red).
REPORTED: the longest tick overall and outside the windows, the long ticks by window, the
sites of every long tick outside, and the FIRST tick after every load window (its work when
long, else "< period").

Exit 77 (SKIP) without the title files, or when the run COMPLETED and reported video=stub.
Needs an IDLE machine (a loaded host stretches ticks by scheduling alone). `--selftest`
judges canned run.meta values.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
PERIOD_US = 16667


def parse_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def parse_windows(s):
    """'60-112,412-412' -> [(60,112),(412,412)]; 'none'/'' -> []."""
    out = []
    for part in (s or "").split(","):
        m = re.fullmatch(r"(\d+)-(\d+)", part.strip())
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def parse_long_trace(s):
    """'ord:us:pending:sites;...' -> [(ord, us, pending, sites)]; 'none' -> []."""
    out = []
    if not s or s == "none":
        return out
    for entry in s.split(";"):
        m = re.match(r"(\d+):(\d+):(\d+):(.*)", entry)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)))
    return out


def inside(ordinal, windows, tail):
    return any(first <= ordinal <= last + tail for first, last in windows)


def judge(returncode, meta, frames):
    checks = []

    def check(ok, what):
        checks.append((ok, what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("video") == "gl", "video=gl (video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    has_rows = all(k in meta for k in ("pace_long", "pace_long_trace", "load_windows", "load_tail_ticks", "fmv_window", "chunk_cuts"))
    check(has_rows, "the long-tick census rows published: pace_long, pace_long_trace, load_windows, load_tail_ticks, fmv_window, "
                    "chunk_cuts (absent on the frozen 91dfdbe6; chunk_cuts absent on baf15a38: the red-before)")
    if not has_rows:
        return checks
    # NO tail: the windows carry the title's chunk transitions and the cut themselves.
    published_tail = int(meta["load_tail_ticks"]) if re.fullmatch(r"\d+", meta["load_tail_ticks"]) else -1
    check(published_tail == 0, "load_tail_ticks=0 (the windows are the title's own load phase; published %s)" % meta["load_tail_ticks"])
    tail = 0
    loads = parse_windows(meta["load_windows"])
    cuts = [int(x) for x in meta["chunk_cuts"].split(":", 1)[-1].split(",") if re.fullmatch(r"\d+", x)]
    cuts_outside = [c for c in cuts if not inside(c, loads, 0)]
    check(not cuts_outside, "every cut tick (a chunk's first Shown update) lies inside a load window (cuts=%s; outside=%s)"
          % (meta["chunk_cuts"], cuts_outside or "none"))
    fmv = parse_windows(meta["fmv_window"])
    longs = parse_long_trace(meta["pace_long_trace"])
    n_long = int(meta["pace_long"]) if re.fullmatch(r"\d+", meta["pace_long"]) else -1
    check(n_long == len(longs) or (n_long > 160 and len(longs) == 160),
          "pace_long=%s agrees with the trace (%d entries; the trace keeps the first 160)" % (meta["pace_long"], len(longs)))
    in_load = [e for e in longs if inside(e[0], loads, tail)]
    in_fmv = [e for e in longs if not inside(e[0], loads, tail) and inside(e[0], [(f - 1, l) for f, l in fmv], tail)]
    outside = [e for e in longs if e not in in_load and e not in in_fmv]
    check(not outside,
          "no tick > %d us outside the load windows (+%d tail) or the FMV window (+%d tail): outside=%s"
          % (PERIOD_US, tail, tail, ["%d:%dus:p%d:%s" % e for e in outside] or "none"))
    longest = max(longs, key=lambda e: e[1]) if longs else None
    longest_out = max(outside, key=lambda e: e[1]) if outside else None
    # The first tick after every load window: a play tick (judged above); reported by name.
    by_ord = {e[0]: e for e in longs}
    after = ["%d:%s" % (last + 1, ("%.1fms" % (by_ord[last + 1][1] / 1000.0)) if last + 1 in by_ord else "<period")
             for _, last in loads]
    checks.append((True, "REPORT long ticks=%d (in load %d, in fmv %d, outside %d); longest %s; longest outside %s; "
                         "first tick after each load window: %s; load_windows=%s tail=%d fmv_window=%s chunk_cuts=%s "
                         "pace_late=%s pace_late_fields=%s frames_dropped=%s"
                   % (len(longs), len(in_load), len(in_fmv), len(outside),
                      ("%d:%.1fms" % (longest[0], longest[1] / 1000.0)) if longest else "none",
                      ("%d:%.1fms" % (longest_out[0], longest_out[1] / 1000.0)) if longest_out else "none",
                      ",".join(after) or "none",
                      meta["load_windows"], tail, meta["fmv_window"], meta["chunk_cuts"], meta.get("pace_late"),
                      meta.get("pace_late_fields"), meta.get("frames_dropped"))))
    return checks


# The s9 shape under the chunk census: the cut 1178 (the chunk's first Shown update, 32.5 ms)
# and the second level's cut 3135 are chunk-event ticks INSIDE the load windows; no tail.
GOOD_META = {
    "frames": "4161", "finalized": "1", "video": "gl", "pace_long": "7",
    "pace_long_trace": "60:252060:3:loader=133377/16,other=116168;111:1365618:0:loader=1293889/2970,other=59076;"
                       "399:1068090:0:gs=557/105,other=1067;412:703136:0:gs=177/72,other=701472;"
                       "1084:18763:1:loader=384/97,alloc=2556/4549,other=14700;"
                       "1178:32521:1:mesh=4316/818,alloc=1074/2411,other=26140;3135:30289:1:mesh=1341/499,alloc=1413/2411,other=27200",
    "load_windows": "60-112,419-419,532-1178,2488-3135", "load_tail_ticks": "0", "fmv_window": "399-412",
    "chunk_cuts": "2:1178,3135", "pace_late": "2", "pace_late_fields": "6", "frames_dropped": "2",
}

# baf15a38's s9 run.meta (play_sessions/s9/run.meta): the 15-tick tail, no chunk_cuts, 1178 past
# the window 532-1175 -- the red-before.
BAF15A38_S9_META = {
    "frames": "4161", "finalized": "1", "video": "gl", "pace_long": "7",
    "pace_long_trace": "60:252060:3:loader=133377/16,other=116168;111:1365618:0:loader=1293889/2970,other=59076;"
                       "399:1068090:0:gs=557/105,other=1067;412:703136:0:gs=177/72,other=701472;"
                       "1084:18763:1:loader=384/97,alloc=2556/4549,other=14700;"
                       "1178:32521:1:mesh=4316/818,alloc=1074/2411,other=26140;3135:30289:1:mesh=1341/499,alloc=1413/2411,other=27200",
    "load_windows": "60-112,419-419,532-1175,2488-3132", "load_tail_ticks": "15", "fmv_window": "399-412",
    "pace_late": "2", "pace_late_fields": "6", "frames_dropped": "1",
}


def selftest():
    fails = 0

    def expect(name, meta, ok_expected, frames=4161, rc=0):
        nonlocal fails
        ok = all(c for c, _ in judge(rc, meta, frames))
        good = ok == ok_expected
        print("selftest %s: %s" % (name, "PASS" if good else "FAIL"))
        fails += not good

    expect("good meta (s9 under the chunk census: the cuts inside the load windows, no tail)", GOOD_META, True)
    expect("baf15a38's s9 meta (tail 15, no chunk_cuts, 1178 past 532-1175) fails: the red-before", BAF15A38_S9_META, False)
    bad = dict(BAF15A38_S9_META); bad["chunk_cuts"] = "0:none"
    expect("a published tail (15) fails even with chunk_cuts", bad, False)
    bad = dict(GOOD_META); bad["load_windows"] = "60-112,419-419,532-1175,2488-3135"
    expect("the cut 1178 outside its window fails (no tail applied)", bad, False)
    bad = dict(GOOD_META); bad["pace_long_trace"] += ";1179:20100:1:other=20000"; bad["pace_long"] = "8"
    expect("the FIRST tick after the load window (1179) long fails", bad, False)
    bad = dict(GOOD_META); bad["pace_long_trace"] += ";2000:20100:1:other=20000"; bad["pace_long"] = "8"
    expect("a 20 ms tick in play (ordinal 2000) fails", bad, False)
    bad = dict(GOOD_META); bad["chunk_cuts"] = "2:1178,2000"
    expect("a cut tick outside every load window fails", bad, False)
    # s3's shape: the files read during the intro movie (load 532-1175 inside fmv 399-2975), the
    # cut 8 ticks after the movie ends (2983) is a chunk-event tick: its own load window.
    s3 = dict(GOOD_META); s3["load_windows"] = "60-112,419-419,532-1175,2983-2983"; s3["fmv_window"] = "399-2975"
    s3["pace_long"] = "3"; s3["pace_long_trace"] = "60:249095:3:loader=120276/16;2975:37234500:0:other=1;2983:53259:3:alloc=2174/2439,other=48427"
    s3["chunk_cuts"] = "1:2983"
    expect("the cut after the movie (s3: 2983 = fmv end + 8, a chunk-event window) passes", s3, True)
    bad = dict(s3); bad["pace_long_trace"] += ";2995:20100:1:other=20000"; bad["pace_long"] = "4"
    expect("a 20 ms tick 20 ticks after the movie fails", bad, False)
    bad = dict(GOOD_META); bad["fmv_window"] = "none"
    expect("FMV holds without a declared fmv_window fail", bad, False)
    bad = dict(GOOD_META); bad["pace_long"] = "9"
    expect("pace_long disagreeing with the trace fails", bad, False)
    frozen = {"frames": "4161", "finalized": "1", "video": "gl", "pace_late_trace": "13:1178:44984:2"}
    expect("the frozen binary's meta (no census rows) fails: the red-before", frozen, False)
    expect("crash exit fails", GOOD_META, False, rc=134)
    none = dict(GOOD_META); none["pace_long"] = "0"; none["pace_long_trace"] = "none"; none["fmv_window"] = "none"
    none["chunk_cuts"] = "0:none"
    expect("no long tick at all passes", none, True)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--padlog")
    ap.add_argument("--frames", type=int, default=3068)
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--keep", help="copy run.meta here")
    ap.add_argument("--extra-set", action="append", default=[], help="additional --set KEY=VALUE for the leg")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner", "padlog"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    out_dir = tempfile.mkdtemp(prefix="root_no_long_tick_")
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir)
    leg = os.path.join(out_dir, "leg")
    os.makedirs(leg)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary, "--profile", "default", "--harness-window",
           "--harness-out", leg, "--frames", str(args.frames), "--inputs", os.path.abspath(args.padlog),
           "--game-files", os.path.abspath(args.game_files), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.log_dir=" + os.path.join(out_dir, "logs")]
    for extra in args.extra_set:
        cmd += ["--set", extra]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(leg, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(leg, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "video", "wall_ms", "pace_parks", "pace_late", "pace_late_fields", "pace_late_trace",
                "pace_work_max_ms", "pace_long", "pace_long_trace", "pace_work_max_breakdown", "tick_profile",
                "load_windows", "load_ticks", "load_tail_ticks", "chunk_ticks", "chunk_cuts", "chunk_state_census", "fmv_window",
                "frames_dropped", "game_hz"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")[:1200]))
    if args.keep and os.path.isfile(meta_path):
        os.makedirs(args.keep, exist_ok=True)
        shutil.copy(meta_path, os.path.join(args.keep, "run.meta"))
    if proc.returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77
    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += not ok
    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("RootNoLongTick: %s" % ("ALL PASS" if failures == 0 else "%d FAIL (artifacts: %s, %s)" % (failures, out_dir, log)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
