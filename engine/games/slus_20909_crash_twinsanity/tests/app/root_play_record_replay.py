#!/usr/bin/env python3
"""RootPlayRecordReplay: a session recorded in PLAY replays deterministically.

The recorder's clock is the GAME FRAME (the frames-lane key of the boundary the pad was
served at; the padlog header names it, `clock=frame`), never the logical input tick -- in
play that tick is wall-paced (a late boundary consumes the elapsed fields, a park the fields
it waited), so a tick-keyed recording of a play session named ticks the deterministic replay
reached at other game frames (the s4 recording sat on the load-file menu).

Three legs on the actual binary:
  A  play profile (`--harness-window`, wall-paced) replays a frame-clock script (`--inputs`)
     for N frames while recording it (`--record-input <file>`);
  B  deterministic headless: `--inputs <A's recording>`;
  C  deterministic headless: `--inputs <the script>`.
The pins:
  * A: finalized=1, frames=N, pad_input=replay, run.meta `pad_record_clock=frame`; its domain
    tick is REPORTED (it exceeds N: the wall-paced count the tick clock would have stamped);
  * A's recording begins with a `# padlog` header naming `clock=frame`, and its per-frame held
    state (buttons, axes) over 1..N equals the script's EXACTLY (the pad is served once per
    frame in every profile, so the frame key is 1:1 -- a shifted range is a FAIL);
  * B and C: both finalized at N frames and IDENTICAL on the frames and audio lanes
    (verify/diff_legs.py), i.e. the recording replays as the script does.
SKIP 77 without the title files, or when leg A COMPLETED with video=stub (no window). A crashed
or killed game process or an unfinalized run.meta is a FAIL. Selftest pins the judge.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "verify"))

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
TITLE = "slus_20909_crash_twinsanity"
# Frame-clock script: short and one-frame presses through the boot (wall-late-prone), an axis
# hold across the logo movies' boundaries, a press on the last frame.
SCRIPT = ["# padlog v1 start_frame=1 clock=frame title=" + TITLE,
          "20-40:CROSS", "100-100:START", "150-170:LX=200", "300-301:TRIANGLE",
          "405-410:CIRCLE", "500-560:RY=40", "590-590:SQUARE"]
SCRIPT_LAST = 590


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


def header_clock(lines):
    """The clock the first `# padlog` header names ('tick' when it names none); None without a header."""
    for raw in lines:
        line = raw.strip()
        if line.startswith("# padlog"):
            for tok in line.split():
                if tok.startswith("clock="):
                    return tok[6:]
            return "tick"
    return None


def held_states(lines, first, last):
    """Per-frame (frozenset(buttons), sorted axes) over [first, last] from padlog range lines
    (the replayer's rule: held over F1..F2 inclusive; the LAST matching axis segment wins)."""
    out = {t: (set(), {}) for t in range(first, last + 1)}
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        for tok in line.split(","):
            tok = tok.strip()
            if ":" not in tok or "-" not in tok.split(":", 1)[0]:
                continue
            rng, what = tok.split(":", 1)
            f1, f2 = rng.split("-", 1)
            try:
                f1, f2 = int(f1), int(f2)
            except ValueError:
                continue
            for t in range(max(f1, first), min(f2, last) + 1):
                if "=" in what:
                    ax, val = what.split("=", 1)
                    out[t][1][ax] = int(val)
                else:
                    out[t][0].add(what)
    return {t: (frozenset(b), tuple(sorted(a.items()))) for t, (b, a) in out.items()}


def judge(frames, play_exit, play_meta, recording, b_exit, b_meta, c_exit, c_meta, lanes_identical, lanes_report):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(play_exit == 0 and play_meta.get("finalized") == "1" and play_meta.get("frames") == str(frames),
          "leg A (play) exit=%s finalized=%s frames=%s (expected 0 / 1 / %d)"
          % (play_exit, play_meta.get("finalized", "<absent>"), play_meta.get("frames", "<absent>"), frames))
    check(play_meta.get("pad_input") == "replay", "leg A pad_input=%s is the script" % play_meta.get("pad_input", "<absent>"))
    check(play_meta.get("pad_record_clock") == "frame",
          "leg A run.meta pad_record_clock=%s (the recorder's clock is the game frame)" % play_meta.get("pad_record_clock", "<absent>"))
    domain = play_meta.get("domain", "")
    tick = None
    for tok in domain.split(","):
        if tok.startswith("tick="):
            tick = int(tok[5:])
    check(tick is not None, "leg A domain tick reported: %s (the wall-paced tick count, %s)"
          % (domain or "<absent>", "> frames: the tick clock would have mis-keyed" if (tick or 0) > frames else "frames"))
    check(header_clock(recording) == "frame", "recording header names clock=frame (got %s)" % header_clock(recording))
    want = held_states(SCRIPT, 1, frames)
    got = held_states(recording, 1, frames)
    differing = [t for t in range(1, frames + 1) if want[t] != got[t]]
    check(not differing, "recording per-frame state == script over frames 1..%d%s"
          % (frames, "" if not differing else " (differs at %d frames, first %s)" % (len(differing), differing[:6])))
    check(b_exit == 0 and b_meta.get("finalized") == "1" and b_meta.get("frames") == str(frames),
          "leg B (deterministic, the recording) exit=%s finalized=%s frames=%s"
          % (b_exit, b_meta.get("finalized", "<absent>"), b_meta.get("frames", "<absent>")))
    check(c_exit == 0 and c_meta.get("finalized") == "1" and c_meta.get("frames") == str(frames),
          "leg C (deterministic, the script) exit=%s finalized=%s frames=%s"
          % (c_exit, c_meta.get("finalized", "<absent>"), c_meta.get("frames", "<absent>")))
    check(lanes_identical, "B and C IDENTICAL on frames and audio (diff_legs)%s"
          % ("" if lanes_identical else ": " + "; ".join(l for l in lanes_report if "FIRST DIVERGENT" in l or "MISMATCH" in l)[:300]))
    return results


def selftest():
    ok = True
    frames = 600
    good_meta = {"finalized": "1", "frames": "600", "pad_input": "replay", "pad_record_clock": "frame",
                 "domain": "tick=1420,absorbed=0,regime=1"}
    det = {"finalized": "1", "frames": "600"}
    r = judge(frames, 0, good_meta, SCRIPT, 0, det, 0, det, True, [])
    ok &= all(x for x, _ in r)
    # the recorder's 60-frame flush splits a hold: the same per-frame state
    split = SCRIPT[:1] + ["20-40:CROSS", "100-100:START", "150-160:LX=200", "161-170:LX=200", "300-301:TRIANGLE",
                          "405-410:CIRCLE", "500-559:RY=40", "560-560:RY=40", "590-590:SQUARE"]
    ok &= all(x for x, _ in judge(frames, 0, good_meta, split, 0, det, 0, det, True, []))
    # a tick-keyed recording of a wall-paced session: ranges shifted / a one-frame press lost
    shifted = SCRIPT[:1] + ["47-91:CROSS", "150-170:LX=200", "300-301:TRIANGLE", "405-410:CIRCLE", "500-560:RY=40", "590-590:SQUARE"]
    fails = [w for x, w in judge(frames, 0, good_meta, shifted, 0, det, 0, det, True, []) if not x]
    ok &= len(fails) == 1 and "per-frame state" in fails[0]
    # the old header (no clock) is a FAIL by name
    old = ["# padlog v1 start_frame=1 title=" + TITLE] + SCRIPT[1:]
    fails = [w for x, w in judge(frames, 0, dict(good_meta, pad_record_clock="tick"), old, 0, det, 0, det, True, []) if not x]
    ok &= len(fails) == 2 and any("clock=frame (got tick)" in w for w in fails) and any("pad_record_clock=tick" in w for w in fails)
    # lanes differ / a leg not finalized / a crashed play run
    fails = [w for x, w in judge(frames, 0, good_meta, SCRIPT, 0, det, 0, det, False, ["frames.fhash: FIRST DIVERGENT key 412"]) if not x]
    ok &= len(fails) == 1 and "FIRST DIVERGENT key 412" in fails[0]
    fails = [w for x, w in judge(frames, 0, good_meta, SCRIPT, 0, {"finalized": "0", "frames": "300"}, 0, det, True, []) if not x]
    ok &= len(fails) == 1 and "leg B" in fails[0]
    fails = [w for x, w in judge(frames, 139, {}, [], 1, {}, 1, {}, False, []) if not x]
    ok &= len(fails) == 9
    print("root_play_record_replay selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run(args, log, argv, progress_file, stall_s):
    cmd = [args.runner, str(args.timeout), log, "--", args.binary] + argv + ["--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = progress_file
    env["RB_STALL_S"] = str(stall_s)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--timeout", type=int, default=240, help="per leg")
    ap.add_argument("--keep", help="copy the three legs' run.meta and the recording here")
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
    if args.frames <= SCRIPT_LAST:
        print("FAIL: --frames must exceed the script's last frame %d" % SCRIPT_LAST)
        return 1
    from diff_legs import diff_legs  # noqa: E402  (verify/)

    root = tempfile.mkdtemp(prefix="root_play_record_replay_")
    script = os.path.join(root, "script.padlog")
    with open(script, "w") as f:
        f.write("\n".join(SCRIPT) + "\n")
    recording = os.path.join(root, "session.padlog")
    a_dir, b_dir, c_dir = (os.path.join(root, n) for n in ("legA", "legB", "legC"))
    for d in (a_dir, b_dir, c_dir):
        os.makedirs(d)
    mc_dir = os.path.join(root, "mc")
    os.makedirs(mc_dir)
    common = ["--frames", str(args.frames), "--set", "paths.mc_dir=" + mc_dir]
    a_exit = run(args, os.path.join(root, "legA.log"),
                 ["--profile", "default", "--harness-window", "--harness-out", a_dir, "--inputs", script,
                  "--record-input", recording, "--set", "paths.log_dir=" + os.path.join(root, "logs")] + common,
                 os.path.join(a_dir, "frames.fhash"), 120)
    a_meta = parse_meta(os.path.join(a_dir, "run.meta"))
    for key in ("frames", "finalized", "video", "pad_input", "pad_record_path", "pad_record_clock", "pad_record_start",
                "pad_record_last", "pad_record_lines", "domain", "tick_thread", "pace_late", "pace_parks", "park", "park_detail"):
        print("leg A run.meta %s=%s" % (key, a_meta.get(key, "<absent>")))
    rec_lines = []
    if os.path.isfile(recording):
        with open(recording) as f:
            rec_lines = [l.rstrip("\n") for l in f]
        print("recording (%d lines):" % len(rec_lines))
        for l in rec_lines[:16]:
            print("  " + l)
    if a_exit == 0 and a_meta.get("finalized") == "1" and a_meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display (see %s)" % os.path.join(root, "legA.log"))
        shutil.rmtree(root, ignore_errors=True)
        return 77
    b_exit = c_exit = 1
    b_meta = c_meta = {}
    identical, report = False, []
    if rec_lines:
        b_exit = run(args, os.path.join(root, "legB.log"),
                     ["--profile", "deterministic", "--headless", "--harness-out", b_dir, "--inputs", recording] + common,
                     os.path.join(b_dir, "frames.fhash"), min(args.timeout, 90))
        b_meta = parse_meta(os.path.join(b_dir, "run.meta"))
        c_exit = run(args, os.path.join(root, "legC.log"),
                     ["--profile", "deterministic", "--headless", "--harness-out", c_dir, "--inputs", script] + common,
                     os.path.join(c_dir, "frames.fhash"), min(args.timeout, 90))
        c_meta = parse_meta(os.path.join(c_dir, "run.meta"))
        for name, m in (("B", b_meta), ("C", c_meta)):
            print("leg %s run.meta frames=%s finalized=%s domain=%s park=%s" % (name, m.get("frames", "<absent>"),
                  m.get("finalized", "<absent>"), m.get("domain", "<absent>"), m.get("park", "<absent>")))
        if os.path.isfile(os.path.join(b_dir, "frames.fhash")) and os.path.isfile(os.path.join(c_dir, "frames.fhash")):
            code, report = diff_legs(b_dir, c_dir, allow_build_diff=False, lanes=["frames", "audio"])
            identical = code == 0
            for l in report[-4:]:
                print("diff_legs: " + l)
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        for name, d in (("A", a_dir), ("B", b_dir), ("C", c_dir)):
            if os.path.isfile(os.path.join(d, "run.meta")):
                shutil.copy(os.path.join(d, "run.meta"), os.path.join(args.keep, "run_%s.meta" % name))
        if os.path.isfile(recording):
            shutil.copy(recording, os.path.join(args.keep, "session.padlog"))
        shutil.copy(script, os.path.join(args.keep, "script.padlog"))
    failures = 0
    for ok, what in judge(args.frames, a_exit, a_meta, rec_lines, b_exit, b_meta, c_exit, c_meta, identical, report):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += not ok
    if failures == 0:
        shutil.rmtree(root, ignore_errors=True)
    print("RootPlayRecordReplay: %s" % ("ALL PASS" if failures == 0 else "%d FAIL (artifacts: %s)" % (failures, root)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
