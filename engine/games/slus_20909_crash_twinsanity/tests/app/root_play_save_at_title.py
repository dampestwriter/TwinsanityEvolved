#!/usr/bin/env python3
"""RootPlaySaveAtTitle: the all-on binary captures a savestate AT THE TITLE and the capture restores.

The Graphics capture-time settle. Before
it, every SyncV boundary from the renderer's first owned recording on refused by name
(`graphics: Graphics owned pending=N,failed=0`): the title's frame boundary is Present -> SyncV ->
StartNextFrame -> Link (TR renderer.cpp:436-440), so at SyncV the frame's owned groups are recorded
behind their predecessors and not yet Linked. The settle admits that boundary and publishes the
unlinked digest (GXPD); the restored session's first Link draws that one frame without those groups
and attributes the omission.

Leg A (the capture): a deterministic headless cold boot to a TITLE boundary (default 1300) with the
capture trigger -- `--set harness.save_at_frame=<B>` (the product's F5 path; used when the binary's
`--help` names it) or `--state-every <B>` (the harness checkpoint writer) -- must WRITE the image
(no `.refused`, no `savestate=refused`), with run.meta `graphics_capture_settle=settles=S,
unlinked=U,...,items=I,...` showing the settle admitted an OPEN recording (U > 0, I > 0: the
boundary carried unlinked owned groups; a closed boundary would not have needed the settle).

Leg B (the restore): `--load-state <image> --frames <tail>` on the same binary: `finalized=1`,
`frames=<tail>`, `restored_vsync_frame=<B>`, `park=none`; run.meta `graphics_restore_unlinked=...,
items=I,...` equals leg A's digest and `graphics_refusals_strips` counts exactly I ordering
refusals with detail 5 (kRestoredUnlinked) at the Link producer -- the omission is attributed,
never a silent glitch.

SKIP 77 only without the title files. Selftest pins the judge (no game process).
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


def parse_fields(value):
    """`a=1,b=2,buckets=21:1/26:3` -> dict of the comma-separated k=v fields."""
    out = {}
    for part in (value or "").split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def refusal_detail_count(strips, detail):
    """Count of `graphics_refusals_strips` entries carrying `/detail:<detail>/`: the field names
    first/last sites with `n:<ordinal>`; the total of detail-5 refusals is `last.n - first.n + 1`
    when both are detail 5 and no other detail appears, else judged from `total`."""
    fields = parse_fields(strips)
    total = int(fields.get("total", "0") or 0)
    first, last = fields.get("first", ""), fields.get("last", "")
    tag = "/detail:%d/" % detail
    if total == 0:
        return 0
    if tag in first and tag in last and fields.get("ordering") == str(total):
        return total
    return -1  # mixed attribution: not the pure restored-unlinked set


def judge_capture(meta, boundary, written, refusal_text):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta.get("finalized") == "1", "A: finalized=1")
    check(meta.get("park", "none") == "none", "A: park=none (run.meta park=%s)" % meta.get("park"))
    check(written, "A: the image was written at boundary %d (refusal: %s)" % (boundary, refusal_text or "none"))
    settle = parse_fields(meta.get("graphics_capture_settle", ""))
    unlinked = int(settle.get("unlinked", "0") or 0)
    items = int(settle.get("items", "0") or 0)
    check(unlinked > 0 and items > 0,
          "A: the settle admitted an OPEN recording (graphics_capture_settle=%s)" % meta.get("graphics_capture_settle"))
    return results, items


def judge_restore(meta, boundary, tail, items):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta.get("finalized") == "1", "B: finalized=1")
    check(meta.get("frames") == str(tail), "B: frames=%d (run.meta frames=%s)" % (tail, meta.get("frames")))
    check(meta.get("restored_vsync_frame") == str(boundary),
          "B: restored_vsync_frame=%d (run.meta %s)" % (boundary, meta.get("restored_vsync_frame")))
    check(meta.get("park", "none") == "none", "B: park=none (run.meta park=%s)" % meta.get("park"))
    restored = parse_fields(meta.get("graphics_restore_unlinked", ""))
    check(restored.get("items") == str(items),
          "B: graphics_restore_unlinked items=%d (run.meta %s)" % (items, meta.get("graphics_restore_unlinked")))
    attributed = refusal_detail_count(meta.get("graphics_refusals_strips", ""), 5)
    check(attributed == items,
          "B: the omission is attributed: %d ordering refusals detail 5 at the Link producer (run.meta %s)"
          % (items, meta.get("graphics_refusals_strips")))
    return results


def selftest():
    ok = True
    good_a = {"finalized": "1", "park": "none",
              "graphics_capture_settle": "settles=32,unlinked=32,source_frame=1784,groups=2,items=4,owned=4,buckets=21:1/26:3"}
    r, items = judge_capture(good_a, 1300, True, "")
    ok &= all(x for x, _ in r) and items == 4
    # the pre-settle shape: refused, nothing admitted
    bad_a = {"finalized": "1", "park": "none", "graphics_capture_settle": "settles=12,unlinked=0,source_frame=0,groups=0,items=0,owned=0,buckets=none"}
    r, _ = judge_capture(bad_a, 1300, False, "graphics: Graphics owned pending=4,failed=0")
    fails = [w for x, w in r if not x]
    ok &= len(fails) == 2 and any("written" in w for w in fails) and any("OPEN recording" in w for w in fails)
    good_b = {"finalized": "1", "frames": "120", "restored_vsync_frame": "1300", "park": "none",
              "graphics_restore_unlinked": "source_frame=1784,groups=2,items=4,owned=4,buckets=21:1/26:3",
              "graphics_refusals_strips": "total=4,missing_resource=0,unsupported_state=0,ordering=4,faults=0,"
                                          "first=frame:0/source_frame:1784/n:1/producer:0x17ed50/detail:5/reason:ordering,"
                                          "last=frame:0/source_frame:1784/n:4/producer:0x17ed50/detail:5/reason:ordering,first_fault=none"}
    ok &= all(x for x, _ in judge_restore(good_b, 1300, 120, 4))
    # a restore that lost the digest or attributes something else
    bad_b = dict(good_b)
    bad_b["graphics_restore_unlinked"] = "source_frame=0,groups=0,items=0,owned=0,buckets=none"
    bad_b["graphics_refusals_strips"] = "total=0,missing_resource=0,unsupported_state=0,ordering=0,faults=0,first=none,last=none,first_fault=none"
    fails = [w for x, w in judge_restore(bad_b, 1300, 120, 4) if not x]
    ok &= len(fails) == 2
    mixed = dict(good_b)
    mixed["graphics_refusals_strips"] = good_b["graphics_refusals_strips"].replace("n:4/producer:0x17ed50/detail:5", "n:4/producer:0x17ed50/detail:4")
    fails = [w for x, w in judge_restore(mixed, 1300, 120, 4) if not x]
    ok &= len(fails) == 1 and "attributed" in fails[0]
    print("root_play_save_at_title selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run(runner, timeout, log, out_dir, args):
    cmd = [runner, str(timeout), log, "--"] + args
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(min(timeout, 120))
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--boundary", type=int, default=1300, help="the title boundary to capture at")
    ap.add_argument("--tail", type=int, default=120, help="frames to run after the capture / after the restore")
    ap.add_argument("--timeout", type=int, default=400)
    ap.add_argument("--trigger", choices=["auto", "save_at_frame", "state-every"], default="auto")
    ap.add_argument("--keep", action="store_true", help="keep the legs on success")
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
    trigger = args.trigger
    if trigger == "auto":
        helptext = subprocess.run([args.binary, "--help"], capture_output=True, text=True).stdout
        trigger = "save_at_frame" if "harness.save_at_frame" in helptext else "state-every"
    print("trigger: %s" % trigger)

    root = tempfile.mkdtemp(prefix="root_play_save_at_title_")
    game = ["--game-files", os.path.abspath(args.game_files)]
    common = [args.binary, "--profile", "deterministic", "--headless"]
    # ---- leg A: the capture at the title boundary ------------------------------------------
    a_dir = os.path.join(root, "capture")
    os.makedirs(a_dir)
    frames_a = args.boundary + args.tail
    if trigger == "save_at_frame":
        extra = ["--set", "harness.save_at_frame=%d" % args.boundary, "--set", "paths.save_dir=" + a_dir]
    else:
        extra = ["--state-every", str(args.boundary)]
    run(args.runner, args.timeout, a_dir + ".log", a_dir,
        common + ["--harness-out", a_dir, "--frames", str(frames_a)] + extra + game)
    meta_a = parse_meta(os.path.join(a_dir, "run.meta")) if os.path.isfile(os.path.join(a_dir, "run.meta")) else {}
    refusal = ""
    image = ""
    if trigger == "save_at_frame":
        state = meta_a.get("savestate", "")
        if state.startswith("written,"):
            m = re.search(r"path=([^,]+)", state)
            image = m.group(1) if m else ""
        else:
            refusal = state
    else:
        candidate = os.path.join(a_dir, "state_b%d.ps2st" % args.boundary)
        refused = os.path.join(a_dir, "state_b%d.refused" % args.boundary)
        if os.path.isfile(candidate):
            image = candidate
        elif os.path.isfile(refused):
            refusal = open(refused).read().strip()
    written = bool(image) and os.path.isfile(image)
    for key in ("frames", "finalized", "park", "park_detail", "savestate", "graphics_capture_settle", "graphics_owned_output"):
        print("A run.meta %s=%s" % (key, meta_a.get(key, "<absent>")))
    results, items = judge_capture(meta_a, args.boundary, written, refusal)
    failures = sum(0 if ok else 1 for ok, _ in results)
    for ok, what in results:
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
    if failures:
        print("RootPlaySaveAtTitle: FAIL (%d) -- legs kept at %s" % (failures, root))
        return 1
    # ---- leg B: the restore -------------------------------------------------------------------
    b_dir = os.path.join(root, "restore")
    os.makedirs(b_dir)
    run(args.runner, args.timeout, b_dir + ".log", b_dir,
        common + ["--harness-out", b_dir, "--frames", str(args.tail), "--load-state", os.path.abspath(image)] + game)
    meta_b = parse_meta(os.path.join(b_dir, "run.meta")) if os.path.isfile(os.path.join(b_dir, "run.meta")) else {}
    for key in ("frames", "finalized", "park", "park_detail", "restored_vsync_frame", "graphics_restore_unlinked",
                "graphics_refusals_strips", "graphics_movie_transport", "graphics_owned_output"):
        print("B run.meta %s=%s" % (key, meta_b.get(key, "<absent>")))
    results = judge_restore(meta_b, args.boundary, args.tail, items)
    failures = sum(0 if ok else 1 for ok, _ in results)
    for ok, what in results:
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
    if failures:
        print("RootPlaySaveAtTitle: FAIL (%d) -- legs kept at %s" % (failures, root))
        return 1
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    print("RootPlaySaveAtTitle: PASS (trigger %s, boundary %d, unlinked items %d)" % (trigger, args.boundary, items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
