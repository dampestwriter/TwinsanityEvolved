#!/usr/bin/env python3
"""RootPlayRelaunchRestore: F8 in a PLAY session (a window, wall-paced boundaries) on the actual
binary, end to end -- the key press without a key.

Leg A (PLAY mode, the F5 path: `--profile default --harness-window --frames 100 --set
harness.save_at_frame=60`) writes the slot (`<saves>/savestates/slot0.ps2st`) from a wall-paced
session -- the composition a play-mode F8 can restore (a deterministic-profile slot is refused by
name: `Saved clock/provider selection differs ... providers(saved=...profile=deterministic,
live=...profile=default)`; RootPlaySaveRestore pins the deterministic round trip).
Leg B (play mode: `--profile default --harness-window --frames <frames> --set save.restore_mode=relaunch`
-- the relaunch route BY SETTING; the compiled default is the live in-process restore, pinned by
RootPlayLiveRestore): `--set harness.restore_at_frame=100` submits RestoreReproduction at boundary 100
exactly as F8 does:
with nothing selected the newest slot is taken, preflighted against the live registry on the
game owner, the session closes and `main` re-executes THIS binary (same pid: the bounded runner
keeps following it) with the same arguments plus `--load-state <slot>` and `--harness-out
<out>/restores/<stamp>` (the one-shot trigger dropped). Pins:
  * leg B's stderr (the runner log) carries, in order, `nativeee: savestate F8 restore Queued`,
    `... F8 restore Completed: Restarting from <slot>` and `... F8 restore Relaunch: <argv>` whose
    argv names `--load-state <slot>` and no `harness.restore_at_frame`;
  * the ORIGINAL leg B run.meta (closed by the relaunch) is finalized with `park_detail=session_closed`;
  * exactly one `<out>/restores/<stamp>/run.meta`: `finalized=1`, `frames=<frames>`,
    `restored_vsync_frame=60`, `video=gl`, `park=none` -- the restored play session ran to its cap.
Legs C/D (review loop 1, item 1 -- the composition preflight in the LIVE session): leg C writes a
DETERMINISTIC-profile slot headless (`--profile deterministic --headless --frames 100 --set
harness.save_at_frame=60`) under a second save root; leg D is a play session (`--profile default
--harness-window --frames <refusal_frames> --set harness.restore_at_frame=100`) over that root.
Pins: stderr `F8 restore Queued`, then `F8 restore Refused: <slot>: composition differs:
profile(saved=deterministic,live=default) ...`, NO `Relaunch:` line, no `--load-state` refusal
line, no restores/ dir; and leg D's OWN run.meta is `finalized=1 frames=<refusal_frames>
park=none park_detail=frames_cap video=gl` -- the window stayed open and the session ran to its
cap (before the fix the session closed and the relaunched --load-state exited 2).
SKIP 77 without the title files or a display (video=stub on a completed run). Selftest pins the
judge (no game process).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
F8 = "nativeee: savestate F8 restore "


def parse_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def judge(log_text, meta_b, restores, slot, frames):
    """restores: list of (dir, meta) under <out>/restores."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    lines = [l for l in log_text.splitlines() if l.startswith(F8)]
    phases = [l[len(F8):].split(":", 1)[0] for l in lines]
    check("Queued" in phases, "B: stderr acknowledged the F8 press (`%sQueued`)" % F8)
    check(any(p == "Completed" and slot in l for p, l in zip(phases, lines)),
          "B: stderr `%sCompleted: Restarting from %s`" % (F8, slot))
    relaunch = [l for p, l in zip(phases, lines) if p == "Relaunch"]
    check(len(relaunch) == 1 and "--load-state " + slot in relaunch[0] and "harness.restore_at_frame" not in relaunch[0],
          "B: one `%sRelaunch:` line naming --load-state %s without the one-shot trigger" % (F8, slot))
    check(phases and phases.index("Queued") < phases.index("Completed") < phases.index("Relaunch")
          if {"Queued", "Completed", "Relaunch"} <= set(phases) else False, "B: Queued -> Completed -> Relaunch in that order")
    check(meta_b.get("finalized") == "1" and meta_b.get("park_detail") == "session_closed",
          "B: the pressed session's run.meta finalized with park_detail=session_closed (got finalized=%s park_detail=%s)"
          % (meta_b.get("finalized"), meta_b.get("park_detail")))
    check(len(restores) == 1, "R: exactly one restores/<stamp>/run.meta (found %d)" % len(restores))
    meta_r = restores[0][1] if restores else {}
    check(meta_r.get("finalized") == "1", "R: finalized=1 (run.meta finalized=%s)" % meta_r.get("finalized"))
    check(meta_r.get("frames") == str(frames), "R: frames=%d (run.meta frames=%s)" % (frames, meta_r.get("frames")))
    check(meta_r.get("restored_vsync_frame") == "60", "R: restored_vsync_frame=60 (run.meta %s)" % meta_r.get("restored_vsync_frame"))
    check(meta_r.get("video") == "gl", "R: video=gl (run.meta video=%s)" % meta_r.get("video"))
    check(meta_r.get("park") == "none", "R: park=none (run.meta park=%s)" % meta_r.get("park", "<absent>"))
    return results


def judge_refusal(log_text, meta_d, restores, slot_det, frames):
    """Leg D: the deterministic-profile slot refused in the LIVE play session, the window open."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    lines = [l for l in log_text.splitlines() if l.startswith(F8)]
    phases = [l[len(F8):].split(":", 1)[0] for l in lines]
    check("Queued" in phases, "D: stderr acknowledged the F8 press (`%sQueued`)" % F8)
    refused = [l for p, l in zip(phases, lines) if p == "Refused"]
    check(len(refused) == 1 and slot_det in refused[0] and "composition differs:" in refused[0]
          and "profile(saved=deterministic,live=default)" in refused[0],
          "D: one `%sRefused: %s: composition differs: profile(saved=deterministic,live=default) ...` line" % (F8, slot_det))
    check("Completed" not in phases and "Relaunch" not in phases and "Working" not in phases,
          "D: no Completed/Relaunch (the session was NOT closed for a slot of another composition)")
    check(not any(l.startswith("nativeee: --load-state") for l in log_text.splitlines()),
          "D: no relaunched --load-state refusal line (the preflight caught it in the live session)")
    check(not restores, "D: no restores/<stamp>/ (found %d)" % len(restores))
    check(meta_d.get("finalized") == "1", "D: finalized=1 (run.meta finalized=%s)" % meta_d.get("finalized"))
    check(meta_d.get("frames") == str(frames), "D: frames=%d -- the session ran to its cap (run.meta frames=%s)" % (frames, meta_d.get("frames")))
    check(meta_d.get("park") == "none" and meta_d.get("park_detail") == "frames_cap",
          "D: park=none park_detail=frames_cap -- the window stayed open (got park=%s park_detail=%s)"
          % (meta_d.get("park", "<absent>"), meta_d.get("park_detail")))
    check(meta_d.get("video") == "gl", "D: video=gl (run.meta video=%s)" % meta_d.get("video"))
    return results


def selftest():
    ok = True
    slot = "/o/saves/savestates/slot0.ps2st"
    log = (F8 + "Queued: Queued for the game owner.\n" +
           F8 + "Completed: Restarting from " + slot + " (--load-state).\n" +
           F8 + "Relaunch: /b/nativeee --profile default --harness-window --harness-out /o/restores/5 --frames 400 "
           "--set paths.save_dir=/o/saves --load-state " + slot + "\n")
    meta_b = {"finalized": "1", "park_detail": "session_closed"}
    good_r = {"finalized": "1", "frames": "400", "restored_vsync_frame": "60", "video": "gl", "park": "none"}
    r = judge(log, meta_b, [("/o/restores/5", good_r)], slot, 400)
    ok &= all(x for x, _ in r)
    # the s3 shape: F8 does nothing -- no lines, no restore
    r = judge("", {"finalized": "1", "park_detail": "frames_cap"}, [], slot, 400)
    ok &= sum(1 for x, _ in r if not x) == 11
    # a relaunch that carried the trigger would loop: refused
    bad = log.replace("--load-state " + slot, "--set harness.restore_at_frame=100 --load-state " + slot)
    r = judge(bad, meta_b, [("/o/restores/5", good_r)], slot, 400)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "one-shot" in w]
    # the restored session crashed
    r = judge(log, meta_b, [("/o/restores/5", {"finalized": "1", "frames": "17", "park": "exception", "video": "gl",
                                              "restored_vsync_frame": "60"})], slot, 400)
    ok &= sum(1 for x, _ in r if not x) == 2
    # legs C/D: the deterministic slot refused in the live session
    slot_det = "/o/saves_det/savestates/slot0.ps2st"
    log_d = (F8 + "Queued: Queued for the game owner.\n" +
             F8 + "Refused: " + slot_det + ": composition differs: profile(saved=deterministic,live=default) -- "
             "the slot was captured under another composition; its --load-state would refuse after this session "
             "closed, so the session stays open\n")
    meta_d = {"finalized": "1", "frames": "160", "park": "none", "park_detail": "frames_cap", "video": "gl"}
    r = judge_refusal(log_d, meta_d, [], slot_det, 160)
    ok &= all(x for x, _ in r)
    # the r6a shape (before the fix): accepted, session closed, relaunch exited 2
    log_old = (F8 + "Queued: Queued for the game owner.\n" + F8 + "Completed: Restarting from " + slot_det + " (--load-state).\n" +
               F8 + "Relaunch: /b/nativeee --load-state " + slot_det + "\n" +
               "nativeee: --load-state " + slot_det + ": Saved clock/provider selection differs from the running session\n")
    r = judge_refusal(log_old, {"finalized": "1", "frames": "100", "park": "none", "park_detail": "session_closed", "video": "gl"},
                      [("/o/restores/9", {"finalized": "1", "frames": "0"})], slot_det, 160)
    ok &= sum(1 for x, _ in r if not x) == 6
    # refused for the wrong reason (a missing section, not the composition)
    r = judge_refusal(log_d.replace("composition differs: profile(saved=deterministic,live=default)",
                                    "required section missing: GCLK (SessionClock)"), meta_d, [], slot_det, 160)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "composition differs" in w]
    print("root_play_relaunch_restore selftest: %s" % ("PASS" if ok else "FAIL"))
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
    ap.add_argument("--frames", type=int, default=400, help="leg B's cap (also the restored session's)")
    ap.add_argument("--refusal-frames", type=int, default=160, help="leg D's cap (the refused press at boundary 100)")
    ap.add_argument("--timeout", type=int, default=240)
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
    root = tempfile.mkdtemp(prefix="root_play_relaunch_restore_")
    saves = os.path.join(root, "saves")
    leg_a = os.path.join(root, "leg_a")
    leg_b = os.path.join(root, "leg_b")
    os.makedirs(leg_a)
    os.makedirs(leg_b)
    gf = os.path.abspath(args.game_files)
    code_a = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", leg_a,
                        "--game-files", gf, "--frames", "100", "--set", "harness.save_at_frame=60",
                        "--set", "paths.save_dir=" + saves], os.path.join(root, "leg_a.log"), os.path.join(leg_a, "frames.fhash"))
    slot = os.path.join(saves, "savestates", "slot0.ps2st")
    meta_a = parse_meta(os.path.join(leg_a, "run.meta")) if os.path.isfile(os.path.join(leg_a, "run.meta")) else {}
    if os.path.isfile(os.path.join(root, "leg_a.log")):
        for line in open(os.path.join(root, "leg_a.log")).read().splitlines():
            if line.startswith("nativeee: savestate"):
                print("stderr A: " + line[:300])
    if code_a == 0 and meta_a.get("finalized") == "1" and meta_a.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    if code_a != 0 or not os.path.isfile(slot):
        print("FAIL: leg A wrote no slot (runner exit %d; run.meta savestate=%s) -- kept at %s" %
              (code_a, meta_a.get("savestate", "<absent>"), root))
        return 1
    log_b = os.path.join(root, "leg_b.log")
    # The relaunch route by setting (LIVE is the default: RootPlayLiveRestore pins it;
    # this leg pins the fallback route the setting keeps reachable).
    code_b = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", leg_b,
                        "--game-files", gf, "--frames", str(args.frames), "--set", "harness.restore_at_frame=100",
                        "--set", "save.restore_mode=relaunch",
                        "--set", "paths.save_dir=" + saves], log_b, os.path.join(leg_b, "frames.fhash"))
    log_text = open(log_b).read() if os.path.isfile(log_b) else ""
    for line in log_text.splitlines():
        if line.startswith("nativeee: savestate") or line.startswith("nativeee: --load-state"):
            print("stderr: " + line[:300])
    meta_b = parse_meta(os.path.join(leg_b, "run.meta")) if os.path.isfile(os.path.join(leg_b, "run.meta")) else {}
    restores = []
    rdir = os.path.join(leg_b, "restores")
    if os.path.isdir(rdir):
        for stamp in sorted(os.listdir(rdir)):
            mp = os.path.join(rdir, stamp, "run.meta")
            if os.path.isfile(mp):
                restores.append((os.path.join(rdir, stamp), parse_meta(mp)))
    for key in ("frames", "finalized", "park", "park_detail", "video"):
        print("run.meta B %s=%s" % (key, meta_b.get(key, "<absent>")))
    for d, m in restores:
        for key in ("frames", "finalized", "restored_vsync_frame", "video", "park", "park_detail", "wall_ms"):
            print("run.meta R(%s) %s=%s" % (os.path.basename(d), key, m.get(key, "<absent>")))
    if code_b == 0 and meta_b.get("finalized") == "1" and meta_b.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    failures = 0
    for ok, what in judge(log_text, meta_b, restores, slot, args.frames):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if code_b != 0:
        print("  FAIL B: runner exit %d (the relaunched session must complete)" % code_b)
        failures += 1
    # Legs C/D: a deterministic-profile slot is refused BY NAME in the live play session.
    saves_det = os.path.join(root, "saves_det")
    leg_c = os.path.join(root, "leg_c")
    leg_d = os.path.join(root, "leg_d")
    os.makedirs(leg_c)
    os.makedirs(leg_d)
    code_c = run(args, [args.binary, "--profile", "deterministic", "--headless", "--harness-out", leg_c,
                        "--game-files", gf, "--frames", "100", "--set", "harness.save_at_frame=60",
                        "--set", "paths.save_dir=" + saves_det], os.path.join(root, "leg_c.log"), os.path.join(leg_c, "frames.fhash"))
    slot_det = os.path.join(saves_det, "savestates", "slot0.ps2st")
    meta_c = parse_meta(os.path.join(leg_c, "run.meta")) if os.path.isfile(os.path.join(leg_c, "run.meta")) else {}
    if code_c != 0 or not os.path.isfile(slot_det):
        print("  FAIL C: the deterministic leg wrote no slot (runner exit %d; run.meta savestate=%s)" %
              (code_c, meta_c.get("savestate", "<absent>")))
        failures += 1
    else:
        log_d = os.path.join(root, "leg_d.log")
        code_d = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", leg_d,
                            "--game-files", gf, "--frames", str(args.refusal_frames), "--set", "harness.restore_at_frame=100",
                            "--set", "save.restore_mode=relaunch",
                            "--set", "paths.save_dir=" + saves_det], log_d, os.path.join(leg_d, "frames.fhash"))
        log_text_d = open(log_d).read() if os.path.isfile(log_d) else ""
        for line in log_text_d.splitlines():
            if line.startswith("nativeee: savestate") or line.startswith("nativeee: --load-state"):
                print("stderr D: " + line[:400])
        meta_d = parse_meta(os.path.join(leg_d, "run.meta")) if os.path.isfile(os.path.join(leg_d, "run.meta")) else {}
        restores_d = []
        rdir_d = os.path.join(leg_d, "restores")
        if os.path.isdir(rdir_d):
            for stamp in sorted(os.listdir(rdir_d)):
                if os.path.isfile(os.path.join(rdir_d, stamp, "run.meta")):
                    restores_d.append(os.path.join(rdir_d, stamp))
        for key in ("frames", "finalized", "park", "park_detail", "video"):
            print("run.meta D %s=%s" % (key, meta_d.get(key, "<absent>")))
        for ok, what in judge_refusal(log_text_d, meta_d, restores_d, slot_det, args.refusal_frames):
            print("  %s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
        if code_d != 0:
            print("  FAIL D: runner exit %d (the refused session must run to its cap)" % code_d)
            failures += 1
    if failures:
        print("RootPlayRelaunchRestore: FAIL (%d) -- legs kept at %s" % (failures, root))
        return 1
    if args.keep:
        print("legs kept at %s" % root)
    else:
        shutil.rmtree(root, ignore_errors=True)
    print("RootPlayRelaunchRestore: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
