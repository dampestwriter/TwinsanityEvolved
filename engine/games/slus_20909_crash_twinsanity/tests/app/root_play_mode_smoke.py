#!/usr/bin/env python3
"""RootPlayModeSmoke -- the composition root starts the play-mode ticker
(INTEGRATION_PLAN row 7.6 stage; INT-I71A-PLAY-MODE-STALL-F60): a windowed,
wall-paced coldboot (`--frames N --harness-window`, N > 60) must run past
frame 60 -- the game's first kernel timer wait completes only when a 60 Hz
pacer notes ticks the waiter fires -- and end at its --frames cap. run.meta:

  * frames=N, finalized=1              -- the cap ended the leg (no stall)
  * tick_thread=ticks=<k>,resyncs=<r>  -- the TickThread ran (k > 0)
  * audio_sink=sdl, pad_input=live     -- the play-mode devices of row 7.1/7.5
  * video=gl                           -- a window existed
  * fmv_active_frames=<n>              -- row 7.6's census key is published
  * savestate_refused_fmv=<n>

Verdict rules (cutover 2026-10-08: a crashed game process is never a skip):
  * FAIL whenever the runner exits non-zero (a crash, SIGABRT 134, a kill by
    run_bounded.sh 124/125/137) or run.meta is missing or not finalized=1 --
    whatever else run.meta says or does not say.
  * SKIP 77 only for the documented missing-asset case (the title files are
    absent) or for a display-less host: the run COMPLETED (exit 0, finalized=1)
    and the root reported video=stub (no window could be created).
`--selftest` runs this judge against a fake runner (tests/app/smoke_fake_runner.py):
exit 134 with an unfinalized run.meta -> FAIL; exit 0 with no run.meta -> FAIL;
exit 0 finalized=0 -> FAIL; a finalized stub-video run -> SKIP; a good run -> PASS.
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


def completed_without_window(returncode, meta):
    """The only non-asset SKIP: the game process ran to its cap and finalized, but the root
    reported video=stub (a display-less host). A dead or unfinalized process never qualifies."""
    return returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub"


def judge(returncode, meta, frames):
    """Returns [(ok, what)] for a completed-or-not game process; the exit code and
    finalized=1 pins come first so a crash reads as its own failure."""
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("video") == "gl", "video=gl (a window existed; video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (the --frames cap ended the leg)" % frames)
    m = re.fullmatch(r"ticks=(\d+),resyncs=(\d+)", meta.get("tick_thread", ""))
    check(m is not None and int(m.group(1)) > 0, "tick_thread=ticks=<k>,resyncs=<r> with k > 0 (the ticker ran)")
    check(meta.get("audio_sink") == "sdl", "audio_sink=sdl (play mode)")
    check(meta.get("pad_input") == "live", "pad_input=live (play mode)")
    check(re.fullmatch(r"\d+", meta.get("fmv_active_frames", "")) is not None, "fmv_active_frames=<n> published")
    check(re.fullmatch(r"\d+", meta.get("savestate_refused_fmv", "")) is not None, "savestate_refused_fmv=<n> published")
    return checks


GOOD_META = ("frames=120\nfinalized=1\nvideo=gl\ntick_thread=ticks=221,resyncs=0\naudio_sink=sdl\n"
             "pad_input=live\nfmv_active_frames=0\nsavestate_refused_fmv=0\n")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    frames = ["--frames", "120"]
    cases = [
        # the flip-2/3 shape: SIGABRT in the game, run.meta written but never finalized
        ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames, 1, "FAIL bounded run exits 0"),
        ("crash exit 134, run.meta with video=gl", 134, "frames=64\nfinalized=0\nvideo=gl\n", frames, 1, "RootPlayModeSmoke: FAIL"),
        ("crash exit 134, no run.meta", 134, None, frames, 1, "RootPlayModeSmoke: FAIL"),
        ("runner kill 124 (wall timeout)", 124, "frames=30\nfinalized=0\nvideo=gl\n", frames, 1, "RootPlayModeSmoke: FAIL"),
        ("exit 0, no run.meta", 0, None, frames, 1, "FAIL finalized=1"),
        ("exit 0, finalized=0", 0, "frames=120\nfinalized=0\nvideo=gl\n", frames, 1, "FAIL finalized=1"),
        ("exit 0, finalized but stub video (display-less host)", 0, "frames=120\nfinalized=1\nvideo=stub\n", frames, 77, "SKIP: no window"),
        ("exit 134 with stub video is still a crash", 134, "frames=3\nfinalized=0\nvideo=stub\n", frames, 1, "RootPlayModeSmoke: FAIL"),
        ("good run", 0, GOOD_META, frames, 0, "RootPlayModeSmoke: ALL PASS"),
        ("good run, wrong frame cap", 0, GOOD_META, ["--frames", "121"], 1, "FAIL frames=121"),
    ]
    ok = fake.expect(cases, script)
    print("root_play_mode_smoke selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--selftest", action="store_true", help="judge a fake runner (no game process)")
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

    out_dir = tempfile.mkdtemp(prefix="root_play_smoke_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--harness-window", "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "40"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in ("frames", "finalized", "tick_thread", "audio_sink", "pad_input", "video", "fmv_active_frames",
                "savestate_refused_fmv", "wall_ms"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if completed_without_window(proc.returncode, meta):
        print("SKIP: no window (video=stub, run completed and finalized): a play smoke needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77

    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootPlayModeSmoke: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
