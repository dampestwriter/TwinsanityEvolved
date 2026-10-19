#!/usr/bin/env python3
"""RootScreenModeSwap -- a display change NEVER blocks the game
thread. The play-profile, wall-paced s3 replay (`--harness-window --inputs
fixtures/play_beach_session.padlog --frames N`) runs a scripted sequence of
Display-page changes (`--set harness.display_sequence=<frame>:<mode>,...`:
windowed / fullscreen / borderless, the internal scale, vsync) submitted through
the SAME SessionControls route the in-game menu widget takes; the presenter's
thread executes each swap between two presented frames (the SDL window / context
calls on the thread that owns them -- on macOS the Spaces fullscreen transition
blocks that thread for its ~0.5-1 s animation) while the game keeps ticking at
60 Hz with the last frame held. The run must show

  * the clock contract itself (RootClockContract's judge: rules 1/3, 2, 4, 5);
  * display_swap=count,max_ms: count == the sequence's presenter steps (every mode /
    vsync change ran on the presenter's thread), max_ms REPORTED;
  * display_sequence submitted == steps, skipped=0: every scripted change reached
    the presenter;
  * display_rows fullscreen= / vsync= / scale= read the sequence's final values;
  * the game thread's longest frame work from the first change on
    (display_sequence work_max_ms_after) < --max-work-ms (100: a swap is several
    hundred ms; the title's own late ticks on this route are < 60 ms) AND no
    pace_late_trace entry at or after the first change's boundary reaches it --
    the s8 red: `pace_late_trace 4593:920142 ... 4897:561157`, the game thread
    blocked for the whole transition;
  * game_hz >= --min-game-hz (50) in EVERY 1 s window from the first change's
    window to the end (a 560 ms stall reads 27; the route's own <=100 ms late
    ticks cost <= 6 boundaries of a window);
  * audio: swap_padded=0 -- no pad of the ring inside any SWAP WINDOW (from a
    presenter step's submission to 6 boundaries after the presenter reported it
    done; display_sequence swap_closed == presenter steps, swap_open=0); pads of
    the title's own late ticks elsewhere in the run (the s3 route's level
    transition at ~1178 pads some runs) are the clock contract's rule 4, not the
    swap's -- padded_after is REPORTED; dropped=0, underruns=0; frame_waits=0;
  * the LIVE PAD PROBE ran (`--set harness.live_pad_probe=on`: the replay serves
    while the live PlatformInput is read at every boundary exactly as a live
    session reads it -- its round trip to the main thread is the product path the
    s8 swap blocked): pad_input=replay+probe, pad_live samples >= frames/2.

Exit 77 (SKIP) without the title files, or when the run COMPLETED and reported
video=stub (no window). A crashed/killed game process or an unfinalized run.meta
is a FAIL. `--selftest` judges canned run.meta values (no game process).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_clock_contract as clock  # noqa: E402  (parse_meta / field / series / judge)

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
DEFAULT_SEQUENCE = ("600:borderless,720:windowed,840:fullscreen,960:windowed,1080:scale2,1200:vsync,"
                    "1320:borderless,1440:scale1,1560:novsync,1680:windowed")
MODE_INDEX = {"windowed": 0, "fullscreen": 1, "borderless": 2}


def parse_sequence(text):
    steps = []
    for item in text.split(","):
        frame, mode = item.split(":", 1)
        steps.append((int(frame), mode))
    return steps


def final_state(steps):
    mode, scale, vsync = 0, 1, 0
    for _, m in steps:
        if m in MODE_INDEX:
            mode = MODE_INDEX[m]
        elif m.startswith("scale"):
            scale = int(m[5:])
        elif m == "vsync":
            vsync = 1
        elif m == "novsync":
            vsync = 0
    return mode, scale, vsync


def presenter_steps(steps):
    return sum(1 for _, m in steps if not m.startswith("scale"))


def late_trace(meta):
    """pace_late_trace 'count:ord:work_us:pending,ord:work_us:pending,...' -> [(ord, work_us, pending)]."""
    text = meta.get("pace_late_trace", "")
    if ":" not in text:
        return []
    body = text.split(":", 1)[1]
    out = []
    for item in body.split(","):
        parts = item.split(":")
        if len(parts) == 3 and all(re.fullmatch(r"\d+", p) for p in parts):
            out.append(tuple(int(p) for p in parts))
    return out


def judge(returncode, meta, frames, steps, max_work_ms=100, min_game_hz=50, max_own_time_windows=3):
    checks = list(clock.judge(returncode, meta, frames, max_own_time_windows))

    def check(ok, what):
        checks.append((ok, what))

    swap = meta.get("display_swap", "")
    m = re.match(r"(\d+),(\d+)", swap)
    swaps, swap_max_ms = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
    want = presenter_steps(steps)
    check(swaps == want, "display_swap count %d == %d presenter steps (every mode / vsync change ran on the presenter's thread; max %d ms)"
          % (swaps, want, swap_max_ms))
    seq = meta.get("display_sequence", "")
    check(clock.field(seq, "steps") == len(steps) and clock.field(seq, "submitted") == len(steps) and clock.field(seq, "skipped") == 0,
          "display_sequence steps=%d submitted=%d skipped=%d (every scripted change submitted; want %d)"
          % (clock.field(seq, "steps"), clock.field(seq, "submitted"), clock.field(seq, "skipped"), len(steps)))
    mode, scale, vsync = final_state(steps)
    rows = meta.get("display_rows", "")
    check(clock.field(rows, "fullscreen") == mode and clock.field(rows, "scale") == scale and clock.field(rows, "vsync") == vsync,
          "display_rows fullscreen=%d scale=%d vsync=%d read the sequence's final values (%d/%d/%d)"
          % (clock.field(rows, "fullscreen"), clock.field(rows, "scale"), clock.field(rows, "vsync"), mode, scale, vsync))
    # The game thread's frame work since the first change: the census's maximum and
    # the late trace's entries at or after the first change's boundary.
    first_frame = clock.field(seq, "first_frame")
    work_after = clock.field(seq, "work_max_ms_after")
    check(first_frame > 0 and 0 <= work_after < max_work_ms,
          "game thread: longest frame work from the first change (boundary %d) on = %d ms < %d (a swap never ran on it)"
          % (first_frame, work_after, max_work_ms))
    stalls = [(o, w) for o, w, _ in late_trace(meta) if first_frame > 0 and o >= first_frame and w >= max_work_ms * 1000]
    check(not stalls, "pace_late_trace: no boundary at/after %d with work >= %d ms (stalls: %s)"
          % (first_frame, max_work_ms, ["%d:%d us" % s for s in stalls] or "none"))
    # game_hz from the first change's window on: the window a boundary belongs to
    # is found by the cumulative boundary count.
    game_hz = clock.series(meta.get("game_hz"))
    cumulative, start = 0, None
    for i, g in enumerate(game_hz):
        cumulative += g
        if first_frame > 0 and cumulative >= first_frame:
            start = i
            break
    tail = game_hz[start:] if start is not None else []
    low = [(start + i, g) for i, g in enumerate(tail) if g < min_game_hz]
    check(start is not None and len(tail) >= 3 and not low,
          "game_hz >= %d in every window from the first change's window (%s) to the end (%d windows; low: %s)"
          % (min_game_hz, start, len(tail), ["w%d=%d" % lg for lg in low] or "none"))
    ar = meta.get("audio_ring", "")
    check(clock.field(seq, "swap_padded") == 0 and clock.field(seq, "swap_closed") == want and clock.field(seq, "swap_open") == 0,
          "audio: swap_padded=%d (no pad inside a swap window; swap_closed=%d of %d presenter steps, swap_open=%d; "
          "padded_after=%d since the first change, padded_at_first=%d, run total padded=%d)"
          % (clock.field(seq, "swap_padded"), clock.field(seq, "swap_closed"), want, clock.field(seq, "swap_open"),
             clock.field(seq, "padded_after"), clock.field(seq, "padded_at_first"), clock.field(ar, "padded")))
    pad_live = meta.get("pad_live", "")
    check(meta.get("pad_input") == "replay+probe" and clock.field(pad_live, "samples") >= frames // 2,
          "live pad probe: pad_input=%s pad_live samples=%d >= %d (the live pad's boundary read beside the replay ran)"
          % (meta.get("pad_input"), clock.field(pad_live, "samples"), frames // 2))
    checks.append((True, "REPORT display_swap=%s display_sequence=%s pad_live=%s frames_dropped=%s pace_work_max_ms=%s"
                   % (swap, seq, pad_live, meta.get("frames_dropped"), meta.get("pace_work_max_ms"))))
    return checks


GOOD_META = dict(clock.GOOD_META)
GOOD_META.update({
    "frames": "1800",
    "display_swap": "8,912,total_ms=4100",
    "display_sequence": "steps=10,presenter_steps=8,submitted=10,skipped=0,first_frame=600,padded_at_first=8608,padded_after=0,work_max_ms_after=31,"
                        "swap_padded=0,swap_closed=8,swap_open=0",
    "display_rows": "scale_setting=1,scale=1,scale_requests=2,scale_applied=2,scale_async=2,scale_inline=0,grab=512x448,vsync=0,fullscreen=0,aspect=0,render_rate=60,present_hz=60,presents=1900",
    "pad_input": "replay+probe",
    "pad_live": "samples=1790,drains=40,gamepad=idle",
    "pace_late_trace": "3:61:272938:4,860:43753:2,1247:54466:3",
    "audio_ring": "pushed=1,dropped=0,pulled=1,underruns=0,padded=8608,relatches=7,trimmed=9088,pad_with_tail=1",
    # 30 windows: the loads, then the frame loop at 60 to the end (the first change at boundary 600 lands in window 12).
    "game_hz": "60,45,5,33,60,60,60,60,15,0,0,30,60,60,60,60,60,60,59,60,60,60,60,60,60,60,56,60,60,60",
    "field_hz": "60,45,5,33,60,60,60,60,15,0,0,30,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60",
    "render_fps": "60,45,5,33,60,60,60,60,15,0,0,30,60,60,60,60,60,60,59,60,60,60,60,60,60,60,56,60,60,60",
})
GOOD_STEPS = parse_sequence(DEFAULT_SEQUENCE)


def selftest():
    fails = 0

    def expect(name, ok):
        nonlocal fails
        print("selftest %s: %s" % (name, "PASS" if ok else "FAIL"))
        fails += not ok

    expect("good meta passes", all(c for c, _ in judge(0, GOOD_META, 1800, GOOD_STEPS)))
    # The s8 red: a swap on the game thread -- a 920 ms frame after the first change,
    # the window at 6 boundaries, the pads of the stall.
    bad = dict(GOOD_META)
    bad["display_sequence"] = bad["display_sequence"].replace("work_max_ms_after=31", "work_max_ms_after=920")
    expect("work_max_ms_after=920 fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["pace_late_trace"] = "4:61:272938:4,860:43753:2,1247:54466:3,1321:920142:55"
    expect("late trace 1321:920142 fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["game_hz"] = "60,45,5,33,60,60,60,60,15,0,0,30,60,60,60,60,60,60,59,60,60,60,6,0,60,60,56,60,60,60"
    expect("game_hz 6,0 after the first change fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["display_sequence"] = bad["display_sequence"].replace("swap_padded=0", "swap_padded=43680")
    expect("swap_padded=43680 fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["display_sequence"] = bad["display_sequence"].replace("swap_closed=8,swap_open=0", "swap_closed=7,swap_open=1")
    expect("a swap window never closed fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    good = dict(GOOD_META); good["display_sequence"] = good["display_sequence"].replace("padded_after=0", "padded_after=704")
    expect("a pad outside every swap window (the route's own late tick) passes", all(c for c, _ in judge(0, good, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["display_swap"] = "5,912,total_ms=4100"
    expect("display_swap count 5 of 8 fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["display_sequence"] = bad["display_sequence"].replace("submitted=10", "submitted=9")
    expect("submitted 9 of 10 fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["display_rows"] = bad["display_rows"].replace("fullscreen=0", "fullscreen=2")
    expect("final mode borderless instead of windowed fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["display_rows"] = bad["display_rows"].replace("vsync=0", "vsync=1")
    expect("final vsync on fails", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    # The pads BEFORE the first change (the boot load) are not the swap's: the run total
    # may be > 0 while padded_after=0.
    good = dict(GOOD_META); good["audio_ring"] = good["audio_ring"].replace("padded=8608", "padded=9000")
    expect("run-total pads before the first change pass", all(c for c, _ in judge(0, good, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["gs_queue"] = bad["gs_queue"].replace("frame_waits=0", "frame_waits=2")
    expect("frame_waits=2 fails (the clock contract)", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    expect("crash exit fails", not all(c for c, _ in judge(134, GOOD_META, 1800, GOOD_STEPS)))
    bad = dict(GOOD_META); bad["pad_input"] = "replay"; del bad["pad_live"]
    expect("no live pad probe fails (the pin would not exercise the pad's round trip)", not all(c for c, _ in judge(0, bad, 1800, GOOD_STEPS)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--padlog")
    ap.add_argument("--frames", type=int, default=1800)
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--sequence", default=DEFAULT_SEQUENCE, help="harness.display_sequence value")
    ap.add_argument("--max-work-ms", type=int, default=100)
    ap.add_argument("--min-game-hz", type=int, default=50)
    ap.add_argument("--max-own-time-windows", type=int, default=3)
    ap.add_argument("--keep", help="copy run.meta here")
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
    steps = parse_sequence(args.sequence)
    out_dir = tempfile.mkdtemp(prefix="root_screen_mode_swap_")
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir)
    leg = os.path.join(out_dir, "leg")
    os.makedirs(leg)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary, "--profile", "default", "--harness-window",
           "--harness-out", leg, "--frames", str(args.frames), "--inputs", os.path.abspath(args.padlog),
           "--game-files", os.path.abspath(args.game_files), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.log_dir=" + os.path.join(out_dir, "logs"),
           "--set", "harness.display_sequence=" + args.sequence, "--set", "harness.live_pad_probe=on"]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(leg, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(leg, "run.meta")
    meta = clock.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "video", "wall_ms", "display_swap", "display_sequence", "display_rows", "pace_late",
                "pace_late_fields", "pace_late_trace", "pace_work_max_ms", "gs_queue", "frames_dropped", "audio_ring",
                "game_hz", "field_hz", "render_fps", "pad_input", "pad_live"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if args.keep and os.path.isfile(meta_path):
        os.makedirs(args.keep, exist_ok=True)
        shutil.copy(meta_path, os.path.join(args.keep, "run.meta"))
    if proc.returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77
    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, steps, args.max_work_ms, args.min_game_hz, args.max_own_time_windows):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += not ok
    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("RootScreenModeSwap: %s" % ("ALL PASS" if failures == 0 else "%d FAIL (artifacts: %s, %s)" % (failures, out_dir, log)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
