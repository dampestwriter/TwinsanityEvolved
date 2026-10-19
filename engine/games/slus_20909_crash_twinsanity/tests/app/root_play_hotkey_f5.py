#!/usr/bin/env python3
"""RootPlayHotkeyF5: F5 in a PLAY session (a window, wall-paced) on the actual binary through the
REAL key route -- the s5 play bug "F5 does nothing: savestates/ empty, no F5 stderr line".

One leg: `--profile default --harness-window --frames <frames> --set harness.inject_key=F5@<press>
--set paths.save_dir=<saves>`. At boundary <press> the leg pushes a real SDL_KEYDOWN/SDL_KEYUP pair
for F5 into SDL's own queue, stamped as SDL stamps a physical press (the keyboard-focus window's id;
0 when SDL recorded no focus window -- the shape the s5 session's trace showed for its whole run), so
the press is drained by SessionRuntime's main pump and routed SDL_PollEvent -> SessionWindow ->
SessionGui::ProcessEvent -> SessionControls::HandleFunctionKey -> Submit(SaveReproduction), the
very path a finger's F5 takes. Nothing short-cuts to Submit (RootPlayRelaunchRestore's
`harness.restore_at_frame` does, and so never pinned the key route). Pins:
  * stderr `nativeee: harness.inject_key F5 at boundary <press>: pushed into SDL's queue`;
  * stderr `nativeee: savestate F5 save Queued: ...` -- the immediate acknowledgement of the press --
    followed by `nativeee: savestate F5 save Completed: ...` (the loud result), NO `nativeee: F5
    discarded:` line;
  * `<saves>/savestates/slot0.ps2st` + `.meta` exist (`.meta` frame >= <press>);
  * run.meta: finalized=1, frames=<frames>, video=gl, park=none, and the key-route census `hotkeys=key_events=2,function_keys=1,discarded=0`.
Before the fix (8bd76e91 + the knob), when SDL recorded no keyboard-focus window (macOS did not activate the
process -- the s5 session, and 2/2 retained runs): the press is pushed and drained stamped windowID=0,
`nativeee: F5 discarded: SDL stamped window id 0 (session window N)` is printed, no `savestate F5` line, no
slot, hotkeys discarded=1. When macOS did grant focus the old router passed this leg (the press carried the
window's id), so the Root red is focus-dependent; the deterministic red is the unit pin SessionHotkeyRoute.
SKIP 77 without the title files or a display (video=stub on a completed run). Selftest pins the judge
(no game process).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
F5 = "nativeee: savestate F5 save "
INJECT = "nativeee: harness.inject_key F5 at boundary "
DISCARD = "nativeee: F5 discarded:"


def parse_meta(path):
    meta = {}
    if not os.path.isfile(path):
        return meta
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def parse_census(value):
    out = {}
    for item in (value or "").split(","):
        if "=" in item:
            k, v = item.split("=", 1)
            out[k] = v
    return out


def judge(log_text, meta, slot_exists, slot_meta, press, frames):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    lines = log_text.splitlines()
    inject = [l for l in lines if l.startswith(INJECT)]
    check(len(inject) == 1 and inject[0].startswith(INJECT + str(press)) and "pushed into SDL's queue" in inject[0],
          "one `%s%d: pushed into SDL's queue` line (the synthetic press entered SDL's queue)" % (INJECT, press))
    f5 = [l for l in lines if l.startswith(F5)]
    phases = [l[len(F5):].split(":", 1)[0] for l in f5]
    check("Queued" in phases, "stderr acknowledged the F5 press immediately (`%sQueued`)" % F5)
    check("Completed" in phases, "stderr `%sCompleted: ...` (the slot was written)" % F5)
    check(phases and "Queued" in phases and "Completed" in phases and phases.index("Queued") < phases.index("Completed"),
          "Queued before Completed")
    check(not any(p in ("Refused", "Failed") for p in phases), "no `%sRefused/Failed` (got phases %s)" % (F5, phases))
    discarded = [l for l in lines if l.startswith(DISCARD)]
    check(not discarded, "no `%s` line (got %d: %s)" % (DISCARD, len(discarded), discarded[:1]))
    check(slot_exists, "<saves>/savestates/slot0.ps2st + .meta exist")
    try:
        slot_frame = int(slot_meta.get("frame", "-1"))
    except ValueError:
        slot_frame = -1
    check(slot_frame >= press, "slot .meta frame >= %d (got %s)" % (press, slot_meta.get("frame", "<absent>")))
    check(meta.get("finalized") == "1", "run.meta finalized=1 (got %s)" % meta.get("finalized"))
    check(meta.get("frames") == str(frames), "run.meta frames=%d (got %s)" % (frames, meta.get("frames")))
    check(meta.get("video") == "gl", "run.meta video=gl (got %s)" % meta.get("video"))
    check(meta.get("park") == "none", "run.meta park=none (got %s)" % meta.get("park", "<absent>"))
    hot = parse_census(meta.get("hotkeys"))
    check(hot.get("key_events") == "2" and hot.get("function_keys") == "1",
          "run.meta hotkeys key_events=2,function_keys=1 (the pair was drained by the pump; got %s)" % meta.get("hotkeys", "<absent>"))
    check(hot.get("discarded") == "0", "run.meta hotkeys discarded=0 (got %s)" % hot.get("discarded", "<absent>"))
    return results


def selftest():
    ok = True
    good_log = (INJECT + "300: pushed into SDL's queue\n" + F5 + "Queued: Queued for the game owner.\n" +
                F5 + "Working: capturing\n" + F5 + "Completed: /o/saves/savestates/slot0.ps2st\n")
    good_meta = {"finalized": "1", "frames": "420", "video": "gl", "park": "none",
                 "hotkeys": "key_events=2,function_keys=1,discarded=0"}
    r = judge(good_log, good_meta, True, {"frame": "300"}, 300, 420)
    ok &= all(x for x, _ in r)
    # the s5 shape before the fix: pushed, drained, discarded by the GUI's window-id test
    bad_log = INJECT + "300: pushed into SDL's queue\nnativeee: F5 discarded: SDL stamped window id 0 (session window 2)\n"
    bad_meta = dict(good_meta)
    bad_meta["hotkeys"] = "key_events=2,function_keys=1,discarded=1"
    r = judge(bad_log, bad_meta, False, {}, 300, 420)
    ok &= sum(1 for x, _ in r if not x) == 7
    # a refused capture: the key arrived but no slot
    r = judge(good_log.replace("Completed: /o/saves/savestates/slot0.ps2st", "Refused: graphics: owned pending=3"),
              good_meta, False, {}, 300, 420)
    ok &= sum(1 for x, _ in r if not x) == 5
    # the session crashed after the press
    r = judge(good_log, dict(good_meta, frames="301", park="exception"), True, {"frame": "300"}, 300, 420)
    ok &= sum(1 for x, _ in r if not x) == 2
    print("root_play_hotkey_f5 selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run(args, cmd, log, progress):
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = progress
    env["RB_STALL_S"] = str(min(args.timeout, 120))
    proc = subprocess.run([args.runner, str(args.timeout), log, "--"] + cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=420)
    ap.add_argument("--press", type=int, default=300, help="the boundary the synthetic F5 is pressed at")
    ap.add_argument("--timeout", type=int, default=200)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--selftest", action="store_true")
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
    root = tempfile.mkdtemp(prefix="root_play_hotkey_f5_")
    saves = os.path.join(root, "saves")
    out = os.path.join(root, "leg")
    os.makedirs(out)
    gf = os.path.abspath(args.game_files)
    log = os.path.join(root, "leg.log")
    code = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", out,
                      "--game-files", gf, "--frames", str(args.frames),
                      "--set", "harness.inject_key=F5@%d" % args.press,
                      "--set", "paths.save_dir=" + saves], log, os.path.join(out, "frames.fhash"))
    log_text = open(log).read() if os.path.isfile(log) else ""
    for line in log_text.splitlines():
        if line.startswith("nativeee: savestate") or line.startswith(INJECT) or line.startswith(DISCARD) or line.startswith("[sdl-trace]"):
            print("stderr: " + line[:300])
    meta = parse_meta(os.path.join(out, "run.meta"))
    for key in ("frames", "finalized", "park", "park_detail", "video", "hotkeys"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if code == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    slot = os.path.join(saves, "savestates", "slot0.ps2st")
    slot_exists = os.path.isfile(slot) and os.path.isfile(slot + ".meta")
    slot_meta = parse_meta(slot + ".meta")
    print("slot: %s exists=%s meta frame=%s" % (slot, slot_exists, slot_meta.get("frame", "<absent>")))
    failures = 0
    for ok, what in judge(log_text, meta, slot_exists, slot_meta, args.press, args.frames):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if code != 0:
        print("  FAIL: runner exit %d (the session must run to its cap)" % code)
        failures += 1
    if failures:
        print("RootPlayHotkeyF5: FAIL (%d) -- leg kept at %s" % (failures, root))
        return 1
    print("RootPlayHotkeyF5: PASS (F5 through the real SDL key route wrote %s)" % slot)
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print("kept at %s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
