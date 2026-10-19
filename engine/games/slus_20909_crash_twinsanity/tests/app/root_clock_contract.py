#!/usr/bin/env python3
"""RootClockContract -- the clock contract at the root: a play-profile,
wall-paced replay of the s3 play session (`--harness-window --inputs
fixtures/play_beach_session.padlog --frames N`) must show

  rule 1/3  domain tick - fast_forward_fields + pace_dropped_fields == tick_thread
            ticks (+-2): every wall field the pacer noted reached the title's clock
            exactly once or was DROPPED by the bounded catch-up -- no surplus tick,
            no absorbed field (domain absorbed=0), pace_timeouts=0 (the fast-forward
            fields an engaged qol.fast_forward delivered were never pacer ticks:
            subtracted, review C2 loop 1; absent = 0). Rule 3 in the SINGLE-FIELD
            form: a boundary delivers exactly one field; a late frame's further
            fields are the backlog the following boundaries deliver one each
            (pace_catchup), bounded by kCatchUpMaxFields -- a stall past the cap
            (a load, a host stall) drops the excess (pace_dropped_fields, per window
            pace_dropped_hz: wall time the game does not replay). In every 1 s
            window where the title ran its frame loop (>= 30 boundaries) and no
            field was dropped, the fields delivered (field_hz) are 58..62 (interior
            windows of a frame-loop stretch; the transition windows carry a load's
            deferred fields) -- the game's clock advanced at wall speed; a window
            with drops is a STALL window (named, bounded) whose field_hz is
            60 - dropped (+-2): the stall is visible where it happened, never
            spread as slow motion over the windows after it.
  rule 2    gs_queue frame_waits=0, frame_skip=1, ordered_waits=0 AND sync_wait_ms=0
            (the renderer NEVER blocks the game thread: no boundary wait, no owned
            delivery wait, no EE read of the pipeline image -- review loop 1), the
            bridge's per-group fence stream-ordered (graphics_bridge_drain waited=0),
            frames_dropped published;
  rule 4    audio_ring dropped=0, underruns=0 (pad mode on: padded/relatches/
            trimmed published);
  rule 5    game_hz / field_hz / render_fps series and frames_dropped published;
            DONE band: game_hz in 59.5..60.5 in every in-loop window, except a
            window with dropped fields (pace_dropped_hz > 0: a stall of more than
            kCatchUpMaxFields on the game thread -- the EE's own long frame or the
            host's; with sync_wait_ms=0 / frame_waits=0 the renderer had no part in
            it) or whose fields exceed its boundaries (a timer wait took a field
            mid-frame) -- which is PRINTED (STALL / EE_OWN_TIME) and bounded by
            --max-own-time-windows; mem_lane=off in play (the 34 MB hash every 60
            boundaries was one such frame per second).

Exit 77 (SKIP) without the title files, or when the run COMPLETED and reported
video=stub (no window). A crashed/killed game process or an unfinalized run.meta
is a FAIL. `--selftest` judges a canned run.meta (no game process).
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


def field(row, name):
    m = re.search(r"(?:^|,)%s=(\d+)" % re.escape(name), row or "")
    return int(m.group(1)) if m else -1


def series(s):
    return [int(x) for x in s.split(",") if re.fullmatch(r"\d+", x)] if s and s != "off" else []


def judge(returncode, meta, frames, max_own_time_windows=3):
    checks = []

    def check(ok, what):
        checks.append((ok, what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("video") == "gl", "video=gl (video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    # rules 1/3
    dm = re.fullmatch(r"tick=(\d+),absorbed=(\d+),regime=(\d+)", meta.get("domain", ""))
    tm = re.fullmatch(r"ticks=(\d+),resyncs=(\d+)", meta.get("tick_thread", ""))
    dom_tick = int(dm.group(1)) if dm else -1
    absorbed = int(dm.group(2)) if dm else -1
    ticks = int(tm.group(1)) if tm else -1
    # The fast-forward fields (qol.fast_forward engaged) advanced the domain without a
    # pacer tick: subtracted before the comparison (absent / "off" = 0).
    ff_fields = int(meta.get("fast_forward_fields", "0")) if re.fullmatch(r"\d+", meta.get("fast_forward_fields", "0")) else 0
    # Rule 3 (single-field form): a boundary delivers ONE field; a late frame's
    # further fields are delivered by catch-up boundaries, bounded -- the fields the
    # cap dropped never reached the clock and are named (absent = 0).
    dropped = int(meta.get("pace_dropped_fields", "0")) if re.fullmatch(r"\d+", meta.get("pace_dropped_fields", "0")) else 0
    check(dm is not None and tm is not None and abs(dom_tick - ff_fields + dropped - ticks) <= 2,
          "rule 1/3: domain tick %d - fast_forward_fields %d + pace_dropped_fields %d == tick_thread ticks %d (+-2): every wall field reached the title's clock once"
          % (dom_tick, ff_fields, dropped, ticks))
    check(re.fullmatch(r"\d+", meta.get("pace_catchup", "")) is not None, "rule 3: pace_catchup published (%s)" % meta.get("pace_catchup", "<absent>"))
    check(absorbed == 0, "rule 1/3: domain absorbed=0 (no field absorbed under the wall lock; absorbed=%d)" % absorbed)
    check(meta.get("pace_timeouts") == "0", "rule 1: pace_timeouts=0 (pace_timeouts=%s)" % meta.get("pace_timeouts", "<absent>"))
    check(re.fullmatch(r"\d+", meta.get("pace_late_fields", "")) is not None and "pace_surplus" not in meta,
          "rule 1: pace_late_fields published, pace_surplus retired (late_fields=%s)" % meta.get("pace_late_fields", "<absent>"))
    # rule 2
    gq = meta.get("gs_queue", "")
    check(field(gq, "frame_waits") == 0, "rule 2: gs_queue frame_waits=0 (%d)" % field(gq, "frame_waits"))
    check(field(gq, "frame_skip") == 1, "rule 2: gs_queue frame_skip=1 (%d)" % field(gq, "frame_skip"))
    check(re.fullmatch(r"\d+", meta.get("frames_dropped", "")) is not None, "rule 2: frames_dropped published (%s)" % meta.get("frames_dropped", "<absent>"))
    check(field(gq, "ordered_waits") == 0, "rule 2: gs_queue ordered_waits=0 (no owned delivery wait; %d)" % field(gq, "ordered_waits"))
    check(field(gq, "sync_wait_ms") == 0, "rule 2: gs_queue sync_wait_ms=0 (no EE read of the pipeline image waited; %d ms)" % field(gq, "sync_wait_ms"))
    bd = meta.get("graphics_bridge_drain", "")
    check(field(bd, "waited") == 0, "rule 2: graphics_bridge_drain waited=0 (the group fence is the stream position; stream_ordered=%d waited=%d)" % (field(bd, "stream_ordered"), field(bd, "waited")))
    check(meta.get("mem_lane") == "off", "play: mem_lane=off (the deterministic profile's 34 MB hash is not on the 60 Hz path; mem_lane=%s)" % meta.get("mem_lane", "<absent>"))
    # rule 4
    ar = meta.get("audio_ring", "")
    check(field(ar, "dropped") == 0, "rule 4: audio_ring dropped=0 (%d)" % field(ar, "dropped"))
    check(field(ar, "underruns") == 0, "rule 4: audio_ring underruns=0 (%d)" % field(ar, "underruns"))
    check(field(ar, "pad_with_tail") == 1, "rule 4: pad mode on (padded=%d relatches=%d trimmed=%d)" % (field(ar, "padded"), field(ar, "relatches"), field(ar, "trimmed")))
    # rule 5
    game_hz, field_hz, render_fps = series(meta.get("game_hz")), series(meta.get("field_hz")), series(meta.get("render_fps"))
    check(len(game_hz) > 0 and len(field_hz) == len(game_hz) and len(render_fps) == len(game_hz),
          "rule 5: game_hz / field_hz / render_fps series published (%d windows)" % len(game_hz))
    # The dropped edges per window (rule 3's bounded catch-up); absent on an engine
    # before the single-field form = none dropped.
    dropped_hz = series(meta.get("pace_dropped_hz"))
    if len(dropped_hz) != len(game_hz):
        dropped_hz = [0] * len(game_hz)
    # In-loop windows: the INTERIOR of a frame-loop stretch (this, the previous and the
    # next window all >= 30 boundaries) -- the transition windows carry the fields a load
    # or FMV delivered at its first fixed point.
    rows = list(zip(game_hz, field_hz, render_fps, dropped_hz))
    in_loop = [rows[i] for i in range(1, len(rows) - 1)
               if rows[i][0] >= 30 and rows[i - 1][0] >= 30 and rows[i + 1][0] >= 30]
    check(len(in_loop) >= 5, "the title ran its frame loop in >= 5 windows (%d)" % len(in_loop))
    # Rule 3: the clock at wall speed in every in-loop window without a drop; a
    # STALL window's fields are the edges it answered: 60 - dropped (+-2) -- the
    # stall where it happened, never slow motion spread over the windows after it.
    bad = [f for _, f, _, d in in_loop if d == 0 and not 58 <= f <= 62]
    check(not bad, "rule 3: field_hz 58..62 in every in-loop window without dropped fields (the game's clock at wall speed; out of range: %s)" % (bad or "none"))
    bad_stall = [(f, d) for _, f, _, d in in_loop if d > 0 and not 58 - d <= f <= 62 - d]
    check(not bad_stall, "rule 3: a stall window's field_hz is 60 - dropped (+-2): the dropped wall time is lost where the stall was (out of range (field_hz, dropped): %s)"
          % (bad_stall or "none"))
    if in_loop:
        gmin, gmax = min(g for g, _, _, _ in in_loop), max(g for g, _, _, _ in in_loop)
        rmin, rmax = min(r for _, _, r, _ in in_loop), max(r for _, _, r, _ in in_loop)
        # The DONE band. A window with dropped fields held a stall longer than the
        # catch-up cap (the EE's own long frame or the host's; the renderer had no
        # part: sync_wait_ms=0, frame_waits=0 above); a window whose fields outnumber
        # its boundaries had a timer wait take a field mid-frame. Both are named and
        # bounded, not hidden.
        stall = [(g, f, d) for g, f, _, d in in_loop if d > 0]
        own_time = [(g, f) for g, f, _, d in in_loop if f > g and d == 0]
        out_of_band = [g for g, f, _, d in in_loop if not (59.5 <= g <= 60.5) and not f > g and d == 0]
        check(not out_of_band, "DONE band: game_hz 59.5..60.5 in every in-loop window without a stall or an EE-own-time frame (out of band: %s)" % (out_of_band or "none"))
        check(len(stall) + len(own_time) <= max_own_time_windows,
              "STALL windows (dropped fields: a stall > kCatchUpMaxFields on the game thread) + EE_OWN_TIME windows (fields > boundaries) = %d + %d <= %d: %s %s"
              % (len(stall), len(own_time), max_own_time_windows, ["game_hz=%d field_hz=%d dropped=%d" % gfd for gfd in stall] or "none",
                 ["game_hz=%d field_hz=%d" % gf for gf in own_time] or "none"))
        checks.append((True, "REPORT in-loop windows=%d game_hz min/max=%d/%d render_fps min/max=%d/%d (DONE band 59.5..60.5; stall windows=%d own-time windows=%d); "
                             "gs_queue ordered_waits=%d ordered_wait_ms=%d sync_wait_ms=%d frames_dropped=%s pace_late=%s pace_late_fields=%s pace_late_trace=%s"
                       % (len(in_loop), gmin, gmax, rmin, rmax, len(stall), len(own_time), field(gq, "ordered_waits"), field(gq, "ordered_wait_ms"),
                          field(gq, "sync_wait_ms"), meta.get("frames_dropped"), meta.get("pace_late"), meta.get("pace_late_fields"),
                          meta.get("pace_late_trace", "<absent>"))))
        checks.append((True, "REPORT rule 3 single-field: pace_catchup=%s pace_dropped_fields=%s pace_dropped_hz=%s"
                       % (meta.get("pace_catchup", "<absent>"), dropped, meta.get("pace_dropped_hz", "<absent>"))))
    return checks


GOOD_META = {
    "frames": "1500", "finalized": "1", "video": "gl", "domain": "tick=3300,absorbed=0,regime=1",
    "tick_thread": "ticks=3301,resyncs=0", "pace_timeouts": "0", "pace_late_fields": "12",
    "gs_queue": "mode=thread,policy=pipelined,frame_waits=0,frame_skip=1,ordered_waits=0,ordered_wait_ms=0,sync_wait_ms=0",
    "graphics_bridge_drain": "stream_ordered=120,waited=0", "mem_lane": "off",
    "frames_dropped": "0", "audio_ring": "pushed=1,dropped=0,pulled=1,underruns=0,padded=0,relatches=0,trimmed=0,pad_with_tail=1",
    "game_hz": "60,0,0,59,60,60,60,59,60,60,60", "field_hz": "60,0,0,60,60,60,60,60,60,60,60", "render_fps": "60,60,60,55,58,60,60,59,60,60,60",
    "pace_catchup": "7", "pace_dropped_fields": "0", "pace_dropped_hz": "0,0,0,0,0,0,0,0,0,0,0",
}


def selftest():
    fails = 0
    ok = all(c for c, _ in judge(0, GOOD_META, 1500))
    print("selftest good meta: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(GOOD_META); bad["gs_queue"] = bad["gs_queue"].replace("frame_waits=0", "frame_waits=7")
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest frame_waits=7 fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(GOOD_META); bad["domain"] = "tick=3200,absorbed=50,regime=1"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest absorbed/tick mismatch fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # An engaged fast-forward: the domain ran 180 fields ahead of the pacer; subtracted it passes,
    # the same meta without the fast_forward_fields row fails rule 1/3.
    ff = dict(GOOD_META); ff["domain"] = "tick=3480,absorbed=0,regime=1"; ff["fast_forward_fields"] = "180"
    ok = all(c for c, _ in judge(0, ff, 1500))
    print("selftest fast_forward_fields=180 subtracted passes: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    del ff["fast_forward_fields"]
    ok = not all(c for c, _ in judge(0, ff, 1500))
    print("selftest domain 180 ahead without fast_forward_fields fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(GOOD_META); bad["audio_ring"] = bad["audio_ring"].replace("underruns=0", "underruns=3")
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest underruns=3 fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(GOOD_META); bad["field_hz"] = "60,0,0,60,45,60,60,60,60,60,60"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest field_hz 45 in-loop fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    ok = not all(c for c, _ in judge(134, GOOD_META, 1500))
    print("selftest crash exit fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # Review loop 1: a renderer wait of any kind fails; the DONE band holds per window.
    bad = dict(GOOD_META); bad["gs_queue"] = bad["gs_queue"].replace("sync_wait_ms=0", "sync_wait_ms=17")
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest sync_wait_ms=17 fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(GOOD_META); bad["gs_queue"] = bad["gs_queue"].replace("ordered_waits=0", "ordered_waits=3")
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest ordered_waits=3 fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(GOOD_META); bad["graphics_bridge_drain"] = "stream_ordered=0,waited=120"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest bridge drain waited fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # game_hz 58 with field_hz 58 in-loop: two boundaries AND their fields missing -- not
    # an EE-own-time frame (fields == boundaries), so the band judges it: out of band.
    bad = dict(GOOD_META); bad["game_hz"] = "60,0,0,59,58,60,60,59,60,60,60"; bad["field_hz"] = "60,0,0,60,58,60,60,60,60,60,60"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest game_hz 58 / field_hz 58 in-loop fails (out of band): %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # Own-time windows beyond the bound fail.
    bad = dict(GOOD_META); bad["game_hz"] = "60,0,0,59,59,59,59,59,60,60,60"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest 5 own-time windows > 3 fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    ok = all(c for c, _ in judge(0, bad, 1500, max_own_time_windows=5))
    print("selftest 5 own-time windows <= 5 passes: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # Rule 3, single-field form: a stall window (12 edges dropped) reads game_hz = field_hz = 48 with
    # pace_dropped_hz 12 there -- named and bounded, the identity holds with the drops added back.
    st = dict(GOOD_META); st["domain"] = "tick=3288,absorbed=0,regime=1"; st["pace_dropped_fields"] = "12"
    st["game_hz"] = "60,0,0,59,60,48,60,59,60,60,60"; st["field_hz"] = "60,0,0,60,60,48,60,60,60,60,60"; st["pace_dropped_hz"] = "0,0,0,0,0,12,0,0,0,0,0"
    ok = all(c for c, _ in judge(0, st, 1500))
    print("selftest one stall window (48 = 60 - 12 dropped) passes: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(st); bad["pace_dropped_fields"] = "0"; bad["pace_dropped_hz"] = "0,0,0,0,0,0,0,0,0,0,0"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest the same 48 window without drops fails (out of band, identity): %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(st); bad["field_hz"] = "60,0,0,60,60,40,60,60,60,60,60"; bad["game_hz"] = "60,0,0,59,60,40,60,59,60,60,60"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest a stall window short of 60 - dropped fails (slow motion): %s" % ("PASS" if ok else "FAIL")); fails += not ok
    bad = dict(st); bad["field_hz"] = "60,0,0,60,60,48,68,60,60,60,60"; bad["game_hz"] = "60,0,0,59,60,48,68,59,60,60,60"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest a 68 catch-up window after the stall fails (the fields belong to the stall's edges): %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # Stall windows count against the own-time bound.
    bad = dict(GOOD_META); bad["domain"] = "tick=3280,absorbed=0,regime=1"; bad["pace_dropped_fields"] = "20"
    bad["game_hz"] = "60,0,0,59,55,55,55,55,60,60,60"; bad["field_hz"] = "60,0,0,60,55,55,55,55,60,60,60"; bad["pace_dropped_hz"] = "0,0,0,0,5,5,5,5,0,0,0"
    ok = not all(c for c, _ in judge(0, bad, 1500))
    print("selftest 4 stall windows > 3 fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--padlog")
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--keep", help="copy run.meta here")
    ap.add_argument("--max-own-time-windows", type=int, default=3,
                    help="in-loop windows with an EE-own-time frame (fields > boundaries) tolerated")
    ap.add_argument("--extra-set", action="append", default=[],
                    help="additional --set KEY=VALUE for the leg (the display-rate arm: video.render_rate=display harness.display_hz=120)")
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
    out_dir = tempfile.mkdtemp(prefix="root_clock_contract_")
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
    for key in ("frames", "finalized", "video", "wall_ms", "domain", "tick_thread", "pace_parks", "pace_late", "pace_catchup",
                "pace_late_fields", "pace_dropped_fields", "pace_dropped_hz", "pace_slept_ms", "pace_timeouts", "pace_late_trace", "pace_work_max_ms", "gs_queue",
                "gs_queue_l2l", "graphics_bridge_drain", "mem_lane", "frames_dropped", "audio_ring",
                "game_hz", "field_hz", "render_fps", "display_rows", "hfr_camera"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if args.keep and os.path.isfile(meta_path):
        os.makedirs(args.keep, exist_ok=True)
        shutil.copy(meta_path, os.path.join(args.keep, "run.meta"))
    if proc.returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77
    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, args.max_own_time_windows):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += not ok
    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("RootClockContract: %s" % ("ALL PASS" if failures == 0 else "%d FAIL (artifacts: %s, %s)" % (failures, out_dir, log)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
