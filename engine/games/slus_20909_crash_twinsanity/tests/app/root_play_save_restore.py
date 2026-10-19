#!/usr/bin/env python3
"""RootPlaySaveRestore: the product's savestate slot round trip on the actual binary.

Leg A (the uninterrupted run): a deterministic headless cold boot `--frames <save+tail>` with
`--set harness.save_at_frame=<save>` -- the F5 capture without a key press, on the same
SessionCapture path, writer, refusal text and `.meta` sidecar. Pins: run.meta
`savestate=written,path=<slot>,frame=<F>,boundary=<B>`; the slot file and its `.meta`
(frame, build_id, composition, sections) exist; `finalized=1`.

Leg B (the restored run): `--load-state <slot> --frames <tail>` on the same binary with the same
composition. Pins: `finalized=1`, `frames=<tail>`, and the IDENTITY: the restored run's presented
frames (frames.fhash keys 1..tail) equal the uninterrupted run's frames after the capture
(keys B+d+1 .. B+d+tail for ONE small offset d in [-2, 2], reported) -- the restored title
reproduces the uninterrupted run's own next frames.

`--expect-refusal <owner text>`: the refusal path on a real process. Leg A alone; run.meta
`savestate=refused,...` names the owner text, and the leg log (stderr) carries the LOUD line
`nativeee: savestate harness save Refused: ... the last owner to refuse: <owner text>`.
Nothing silent: a leg with neither `savestate=written` nor `savestate=refused` is a FAIL.

SKIP 77 only without the title files. Selftest pins the judge (no game process).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
LOUD_PREFIX = "nativeee: savestate harness save "


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
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or "," not in line:
                continue
            k, h = line.split(",", 1)
            out[int(k)] = h.strip()
    return out


def parse_savestate(value):
    """`written,path=P,frame=F,boundary=B` -> dict; `refused,<detail>` -> {'state': 'refused', 'detail': ...}."""
    if not value:
        return {}
    state, _, rest = value.partition(",")
    out = {"state": state}
    if state == "written":
        for part in rest.split(","):
            if "=" in part:
                k, v = part.split("=", 1)
                out[k] = v
    else:
        out["detail"] = rest
    return out


def judge_saved(meta, slot_exists, meta_text, build_id):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta.get("finalized") == "1", "A: finalized=1 (run.meta finalized=%s)" % meta.get("finalized"))
    save = parse_savestate(meta.get("savestate", ""))
    check(save.get("state") == "written", "A: run.meta savestate=written (got %s)" % meta.get("savestate", "<absent>"))
    check(save.get("path", "").endswith(".ps2st") and slot_exists, "A: the slot file exists: %s" % save.get("path"))
    check(save.get("boundary", "").isdigit() and save.get("frame", "").isdigit(),
          "A: savestate frame=%s boundary=%s" % (save.get("frame"), save.get("boundary")))
    check("frame=%s\n" % save.get("frame") in meta_text, "A: .meta frame=%s" % save.get("frame"))
    check(("build_id=%s\n" % build_id) in meta_text if build_id else "build_id=" in meta_text,
          "A: .meta build_id matches the binary's --version")
    check("composition=hle." in meta_text and "profile=" in meta_text, "A: .meta composition names hle.* and the profile")
    check("sections=" in meta_text and "GCLK" in meta_text, "A: .meta sections list carries GCLK")
    return results, save


def judge_restored(meta_b, lane_a, lane_b, boundary, tail):
    """The restored run's frames.fhash is keyed by the restored game frame (restored_vsync_frame=B ->
    keys B+1..B+tail-1 after one pre-restore line keyed 1, the black present). Every restored key k
    must hash equal to the uninterrupted run's key k+d for ONE constant shift d in {0, 1, -1}
    (reported; the observed convention is d=1: the restored boundary B+1 presents the picture the
    uninterrupted run keyed B+2)."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta_b.get("finalized") == "1", "B: finalized=1 (run.meta finalized=%s)" % meta_b.get("finalized"))
    check(meta_b.get("frames") == str(tail), "B: frames=%d (run.meta frames=%s)" % (tail, meta_b.get("frames")))
    check(meta_b.get("park", "none") == "none", "B: park=none (run.meta park=%s)" % meta_b.get("park"))
    check(meta_b.get("restored_vsync_frame") == str(boundary),
          "B: restored_vsync_frame=%d (run.meta %s)" % (boundary, meta_b.get("restored_vsync_frame", "<absent>")))
    keys = sorted(k for k in lane_b if k != 1)
    check(len(keys) >= tail - 1 and keys and keys[0] == boundary + 1,
          "B: restored lane carries keys %d.. (%d keys after the pre-restore line)" % (boundary + 1, len(keys)))
    match = None
    for d in (1, 0, -1):
        if keys and all((k + d) in lane_a and lane_b[k].lower() == lane_a[k + d].lower() for k in keys):
            match = d
            break
    first_diff = next((k for k in keys if lane_b[k].lower() != lane_a.get(k + 1, "").lower()), None)
    check(match is not None,
          "B: identity: every restored key k (%d..%d) hashes equal to the uninterrupted key k+d (d=%s; first mismatch at d=1: %s)"
          % (keys[0] if keys else 0, keys[-1] if keys else 0, match, first_diff))
    return results


def judge_refused(meta, log_text, owner):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    save = parse_savestate(meta.get("savestate", ""))
    check(save.get("state") == "refused", "R: run.meta savestate=refused (got %s)" % meta.get("savestate", "<absent>"))
    check(owner in save.get("detail", ""), "R: the refusal names the owner `%s`" % owner)
    loud = [l for l in log_text.splitlines() if l.startswith(LOUD_PREFIX + "Refused")]
    check(any(owner in l for l in loud), "R: stderr carries `%sRefused: ... %s`" % (LOUD_PREFIX, owner))
    check(any(l.startswith(LOUD_PREFIX + "Queued") for l in log_text.splitlines()),
          "R: stderr acknowledged the trigger (`%sQueued`)" % LOUD_PREFIX)
    check(meta.get("finalized") == "1", "R: finalized=1 (the refusal did not end the leg)")
    return results


def selftest():
    ok = True
    good = {"finalized": "1", "savestate": "written,path=/o/savestates/slot0.ps2st,frame=1301,boundary=1300"}
    text = "frame=1301\nwall_ms=1\nbuild_id=B1\ncomposition=hle.a=on;profile=deterministic\nsections=GCLK:v2,SVCS:v1\n"
    r, save = judge_saved(good, True, text, "B1")
    ok &= all(x for x, _ in r) and save["boundary"] == "1300"
    r, _ = judge_saved({"finalized": "1", "savestate": "refused,graphics: Graphics owned pending=2,failed=0"}, False, "", "B1")
    ok &= sum(1 for x, _ in r if not x) >= 5
    lane_a = {k: "h%d" % k for k in range(1, 1500)}
    good_b = {"finalized": "1", "frames": "120", "park": "none", "restored_vsync_frame": "1300"}
    lane_b = {1: "0x0"}
    lane_b.update({k: "h%d" % (k + 1) for k in range(1301, 1420)})   # the observed d=1 convention
    r = judge_restored(good_b, lane_a, lane_b, 1300, 120)
    ok &= all(x for x, _ in r) and "d=1" in r[-1][1]
    lane_b0 = {1: "0x0"}
    lane_b0.update({k: "h%d" % k for k in range(1301, 1420)})
    r = judge_restored(good_b, lane_a, lane_b0, 1300, 120)
    ok &= all(x for x, _ in r) and "d=0" in r[-1][1]
    lane_bad = dict(lane_b)
    lane_bad[1350] = "zz"
    r = judge_restored(good_b, lane_a, lane_bad, 1300, 120)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "identity" in w] and not all(x for x, _ in r)
    ok &= "first mismatch at d=1: 1350" in [w for x, w in r if not x][0]
    r = judge_restored({"finalized": "0", "frames": "17", "park": "exception"}, lane_a, {}, 1300, 120)
    ok &= sum(1 for x, _ in r if not x) == 6
    log = ("nativeee: savestate harness save Queued: harness.save_at_frame=1300 reached\n"
           "nativeee: savestate harness save Refused: No admissible boundary in 120 consecutive boundaries; "
           "the last owner to refuse: graphics: Graphics owned pending=2,failed=0\n")
    r = judge_refused({"finalized": "1", "savestate": "refused,No admissible boundary in 120 consecutive boundaries; "
                       "the last owner to refuse: graphics: Graphics owned pending=2,failed=0"}, log, "graphics: Graphics owned pending")
    ok &= all(x for x, _ in r)
    r = judge_refused({"finalized": "1"}, "", "graphics")
    ok &= sum(1 for x, _ in r if not x) == 4
    r = judge_refused(good, log, "graphics")   # a written slot is NOT a refusal
    ok &= not r[0][0]
    print("root_play_save_restore selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run_leg(args, out_dir, extra, log, timeout):
    cmd = [args.runner, str(timeout), log, "--", args.binary, "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--game-files", os.path.abspath(args.game_files)] + extra + list(args.set_args)
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(min(timeout, 120))
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "wall_ms", "savestate", "restored_vsync_frame", "restore_mhash", "park", "park_detail"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    return proc.returncode, meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--save-at", type=int, default=1300, help="harness.save_at_frame")
    ap.add_argument("--tail", type=int, default=120, help="frames after the capture, both legs")
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--expect-refusal", default="", help="owner text the refusal must name (refusal-path mode)")
    ap.add_argument("--keep", action="store_true", help="keep the legs on success")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("set_args", nargs="*", help="extra `--set key=value` pairs passed to both legs (after `--`)")
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
    build_id = ""
    try:
        build_id = subprocess.run([args.binary, "--version"], capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        pass
    root = tempfile.mkdtemp(prefix="root_play_save_restore_")
    saves = os.path.join(root, "saves")
    leg_a = os.path.join(root, "leg_a")
    leg_b = os.path.join(root, "leg_b")
    os.makedirs(leg_a)
    log_a = os.path.join(root, "leg_a.log")
    code_a, meta_a = run_leg(args, leg_a, ["--frames", str(args.save_at + args.tail),
                                           "--set", "harness.save_at_frame=%d" % args.save_at,
                                           "--set", "paths.save_dir=" + saves], log_a, args.timeout)
    log_text = open(log_a).read() if os.path.isfile(log_a) else ""
    for line in log_text.splitlines():
        if line.startswith("nativeee: savestate"):
            print("stderr: " + line)
    failures = 0
    if args.expect_refusal:
        for ok, what in judge_refused(meta_a, log_text, args.expect_refusal):
            print("  %s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
        if code_a != 0:
            print("  FAIL R: runner exit %d (the leg must complete)" % code_a)
            failures += 1
    else:
        save = parse_savestate(meta_a.get("savestate", ""))
        slot = save.get("path", "")
        slot_exists = bool(slot) and os.path.isfile(slot)
        meta_text = open(slot + ".meta").read() if slot_exists and os.path.isfile(slot + ".meta") else ""
        results, save = judge_saved(meta_a, slot_exists, meta_text, build_id)
        for ok, what in results:
            print("  %s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
        if code_a != 0:
            print("  FAIL A: runner exit %d (the leg must complete)" % code_a)
            failures += 1
        if not failures:
            os.makedirs(leg_b)
            log_b = os.path.join(root, "leg_b.log")
            code_b, meta_b = run_leg(args, leg_b, ["--frames", str(args.tail), "--load-state", slot,
                                                   "--set", "paths.save_dir=" + saves], log_b, args.timeout)
            lane_a = read_lane(os.path.join(leg_a, "frames.fhash")) if os.path.isfile(os.path.join(leg_a, "frames.fhash")) else {}
            lane_b = read_lane(os.path.join(leg_b, "frames.fhash")) if os.path.isfile(os.path.join(leg_b, "frames.fhash")) else {}
            for ok, what in judge_restored(meta_b, lane_a, lane_b, int(save["boundary"]), args.tail):
                print("  %s %s" % ("PASS" if ok else "FAIL", what))
                failures += 0 if ok else 1
            if code_b != 0:
                print("  FAIL B: runner exit %d (the restored leg must complete)" % code_b)
                failures += 1
    if failures:
        print("RootPlaySaveRestore: FAIL (%d) -- legs kept at %s" % (failures, root))
        return 1
    if args.keep:
        print("legs kept at %s" % root)
    else:
        shutil.rmtree(root, ignore_errors=True)
    print("RootPlaySaveRestore: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
