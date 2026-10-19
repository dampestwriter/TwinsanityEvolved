#!/usr/bin/env python3
"""RootPlayLiveRestore: F8 restores LIVE, in the running play session, on the actual all-on binary
 -- no relaunch, the window and the GL device
kept, one game process.

Leg A (PLAY mode, uninterrupted: `--profile default --harness-window --frames <frames_a> --set
harness.save_at_frame=<save_at>`): the F5 path writes the slot at boundary <save_at> (the Graphics
capture settle admits the in-play boundary and publishes its GXPD digest: run.meta
`graphics_capture_settle=...,source_frame=S,groups=G,items=I,...`) and the session runs on to
its cap -- the uninterrupted frames lane.
Leg B (play mode: `--frames <frames_b> --set harness.restore_at_frame=<restore_at>`, the compiled
default route save.restore_mode=live): RestoreReproduction is submitted at boundary <restore_at>
exactly as F8 does; the newest slot is preflighted on the game owner (registry, composition,
every owner, the LiveRestoreGate's four named terms), the boundary thread unwinds at the admitted
boundary, the supervisor joins the workers and the capture writer, stops the audio device,
begins the output segment, applies the image (GraphicsOutput::ResetForRestore cancels the open
recording's owned batches; the MTVU worker's unit mirrors the restored VU1), re-arms the
presenter/audio/pacer and relaunches the restored continuations -- in THIS process. Pins:
  * stderr: `nativeee: savestate F8 restore Queued`, then `... F8 restore Completed: Reproduction
    restored; approved continuations relaunched.`; NO `F8 restore Relaunch:` line and no
    `--load-state` line (the relaunch route never ran), no `<out>/restores/` directory;
  * leg B's own run.meta (the pre-restore segment, closed at the switch): `finalized=1`,
    `frames=<restore_at>+1`, `f8=live`, `output_next=<out>/output-000001` -- the process continued
    into its restored segment instead of closing (the relaunch route's `park_detail=session_closed`);
  * exactly one `<out>/output-000001/run.meta`: `finalized=1`, `restore_route=live`,
    `restored_vsync_frame=<save_at>`, `output_restore_frame=<save_at>`, `frames=<frames_b>-<restore_at>-1`
    (the --frames cap counts the process's boundaries), `park=none`, `park_detail=frames_cap`,
    `video=gl` (the same window), `graphics_live_restore=resets=1,...` (the open recording reset on
    the live session), `graphics_restore_unlinked` source_frame/groups/items EQUAL to leg A's
    capture-settle digest (the restored Link attributes exactly the groups the capture left open),
    `gs_queue ... frame_waits=0` and `audio_ring ... dropped=0,underruns=0` (the clock contract's
    renderer/audio invariants hold across the restore);
  * the restored segment's frames lane starts at key <save_at>+1 and its FIRST frame hashes equal to
    the uninterrupted run's frame <save_at>+1 (the restored boundary presents the picture the
    uninterrupted run keyed there);
  * the agreement of the restored frames <save_at>+1.. with the uninterrupted run's (same key, or
    within +-3 keys) is REPORTED, not asserted: the play profile's wall-paced presentation
    (frame-skip under rule 2, the FMV's wall-clocked decode) makes the frames lane run-to-run
    variant -- two uninterrupted play runs of this stretch agree on a minority of keys -- so a
    byte identity is not a property of play mode; the deterministic identity is RootPlaySaveRestore's.
Leg C (`--load-state <slot> --frames <tail>`, the process restore of the same slot): its
`restore_mhash` (the applied image's RAM hash) must EQUAL the live segment's `restore_mhash` -- the
live owners applied the same image the process route applies; its frames agreement with leg A is
reported beside the live one.
SKIP 77 without the title files or a display (video=stub on a completed run). Selftest pins the judge.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
F8 = "nativeee: savestate F8 restore "
COMPLETED = "Reproduction restored; approved continuations relaunched."


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


def parse_lane(path):
    lane = {}
    if not os.path.isfile(path):
        return lane
    with open(path) as f:
        for line in f:
            parts = line.strip().split(",")
            if len(parts) == 2 and parts[0].isdigit():
                lane[int(parts[0])] = parts[1]
    return lane


def kv(text):
    """'a=1,b=2' -> {'a': '1', 'b': '2'} (the census rows' inner syntax)."""
    out = {}
    for item in text.split(","):
        if "=" in item:
            k, v = item.split("=", 1)
            out[k] = v
    return out


def digest(row):
    d = kv(row or "")
    return (d.get("source_frame"), d.get("groups"), d.get("items"))


def agreement(restored, reference, lo, hi):
    """(same, near, total) of restored[k] vs reference[k] (same) or reference[k+-3] (near), k in [lo, hi)."""
    same = near = total = 0
    for k in range(lo, hi):
        if k not in restored or k not in reference:
            continue
        total += 1
        if restored[k] == reference[k]:
            same += 1
        elif any(reference.get(k + d) == restored[k] for d in range(-3, 4)):
            near += 1
    return same, near, total


def judge(log_text, meta_b, segments, restores, meta_a, frames_a, frames_b, save_at, restore_at, lane_a, lane_seg,
          lane_b_last=None):
    """segments: list of (dir, meta) under <out>/output-*; restores: dirs under <out>/restores;
    lane_b_last: the pre-restore segment's last frame hash (None when unknown)."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    lines = [l for l in log_text.splitlines() if l.startswith(F8)]
    phases = [l[len(F8):].split(":", 1)[0] for l in lines]
    check("Queued" in phases, "B: stderr acknowledged the F8 press (`%sQueued`)" % F8)
    check(any(p == "Completed" and COMPLETED in l for p, l in zip(phases, lines)),
          "B: stderr `%sCompleted: %s`" % (F8, COMPLETED))
    check("Relaunch" not in phases and "Refused" not in phases and "Failed" not in phases,
          "B: no Relaunch/Refused/Failed F8 line (got phases %s)" % phases)
    check(not any(l.startswith("nativeee: --load-state") for l in log_text.splitlines()),
          "B: no --load-state line (the relaunch route never ran)")
    check(not restores, "B: no <out>/restores/ (found %d)" % len(restores))
    # The press is submitted at boundary <restore_at>; the transaction runs in that boundary's or the
    # next boundary's Savestate slot (whichever first admits), so the pre-restore segment closes at
    # <restore_at> or <restore_at>+1 boundaries and the restored segment carries the rest of the cap.
    pre = int(meta_b.get("frames", "0") or 0)
    check(meta_b.get("finalized") == "1" and pre in (restore_at, restore_at + 1),
          "B: the pre-restore segment finalized at frames=%d or %d (got finalized=%s frames=%s)"
          % (restore_at, restore_at + 1, meta_b.get("finalized"), meta_b.get("frames")))
    check(meta_b.get("f8") == "live", "B: run.meta f8=live (got %s)" % meta_b.get("f8", "<absent>"))
    check(len(segments) == 1 and meta_b.get("output_next") == segments[0][0],
          "B: exactly one output-000001/ and run.meta output_next names it (segments=%d output_next=%s)"
          % (len(segments), meta_b.get("output_next", "<absent>")))
    seg = segments[0][1] if segments else {}
    check(seg.get("finalized") == "1", "R: finalized=1 (got %s)" % seg.get("finalized"))
    check(seg.get("restore_route") == "live", "R: restore_route=live (got %s)" % seg.get("restore_route", "<absent>"))
    check(seg.get("restored_vsync_frame") == str(save_at) and seg.get("output_restore_frame") == str(save_at),
          "R: restored_vsync_frame=output_restore_frame=%d (got %s/%s)"
          % (save_at, seg.get("restored_vsync_frame"), seg.get("output_restore_frame")))
    expected_frames = frames_b - pre
    check(pre and seg.get("frames") == str(expected_frames),
          "R: frames=%d (the process cap %d minus the %d pre-restore boundaries; got %s)"
          % (expected_frames, frames_b, pre, seg.get("frames")))
    check(seg.get("park") == "none" and seg.get("park_detail") == "frames_cap",
          "R: park=none park_detail=frames_cap -- the restored session ran to the cap in the same window (got %s/%s)"
          % (seg.get("park", "<absent>"), seg.get("park_detail")))
    check(seg.get("video") == "gl", "R: video=gl (got %s)" % seg.get("video"))
    live = kv(seg.get("graphics_live_restore", ""))
    check(live.get("resets") == "1", "R: graphics_live_restore resets=1 (got %s)" % seg.get("graphics_live_restore", "<absent>"))
    da, dr = digest(meta_a.get("graphics_capture_settle")), digest(seg.get("graphics_restore_unlinked"))
    check(da == dr and dr[2] not in (None, "0"),
          "R: graphics_restore_unlinked digest == leg A's capture settle (A %s, R %s)" % (da, dr))
    gsq = kv(seg.get("gs_queue", ""))
    check(gsq.get("frame_waits") == "0", "R: gs_queue frame_waits=0 (got %s)" % gsq.get("frame_waits"))
    ring = kv(seg.get("audio_ring", ""))
    check(ring.get("dropped") == "0" and ring.get("underruns") == "0",
          "R: audio_ring dropped=0 underruns=0 (got dropped=%s underruns=%s)" % (ring.get("dropped"), ring.get("underruns")))
    first = min(lane_seg) if lane_seg else None
    check(first == save_at + 1, "R: the restored frames lane starts at key %d (got %s)" % (save_at + 1, first))
    # The restored session's first pictures: the picture the uninterrupted run keyed at <save_at>+1
    # (the restored boundary presents it; a present before the consumer completed the first restored
    # frame shows NOTHING -- the no-image field, hash 0 -- never a picture of the previous session),
    # and that picture within the first five restored keys.
    none_hash = "0x0000000000000000"
    expected_first = lane_a.get(save_at + 1)
    got_first = lane_seg.get(save_at + 1)
    check(expected_first is not None and got_first in (expected_first, none_hash),
          "R: the first restored frame (%d) is the uninterrupted run's picture or the no-image field (got %s, uninterrupted %s)"
          % (save_at + 1, got_first, expected_first))
    check(expected_first is not None and any(lane_seg.get(k) == expected_first for k in range(save_at + 1, save_at + 6)),
          "R: the uninterrupted run's frame %d appears within the first five restored frames" % (save_at + 1))
    # No restored frame is the PREVIOUS session's last presented picture (the stale frame the design
    # forbids: the consumer's grab and the held image are forgotten at the join).
    stale = lane_b_last
    check(stale is None or stale == expected_first or not any(v == stale for k, v in lane_seg.items() if k <= save_at + 5),
          "R: no early restored frame is the pre-restore session's last picture (%s)" % stale)
    check(meta_a.get("finalized") == "1" and meta_a.get("frames") == str(frames_a),
          "A: the uninterrupted run finalized at frames=%d (got %s)" % (frames_a, meta_a.get("frames")))
    return results


def judge_process(meta_c, seg):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta_c.get("finalized") == "1" and meta_c.get("park") == "none", "C: the process restore ran (finalized=%s park=%s)"
          % (meta_c.get("finalized"), meta_c.get("park")))
    check(meta_c.get("restore_mhash") not in (None, "") and meta_c.get("restore_mhash") == seg.get("restore_mhash"),
          "C: restore_mhash equal between the live restore and the process restore of the same slot (live %s, process %s)"
          % (seg.get("restore_mhash"), meta_c.get("restore_mhash")))
    return results


def selftest():
    ok = True
    log = (F8 + "Queued: Queued for the game owner.\n" + F8 + "Completed: " + COMPLETED + "\n")
    seg_dir = "/o/leg_b/output-000001"
    meta_a = {"finalized": "1", "frames": "600", "graphics_capture_settle": "settles=32,unlinked=32,source_frame=239,groups=3,items=12,owned=12,buckets=0:1"}
    meta_b = {"finalized": "1", "frames": "601", "f8": "live", "output_next": seg_dir}
    seg = {"finalized": "1", "restore_route": "live", "restored_vsync_frame": "300", "output_restore_frame": "300",
           "frames": "299", "park": "none", "park_detail": "frames_cap", "video": "gl",
           "graphics_live_restore": "resets=1,cancelled=2",
           "graphics_restore_unlinked": "source_frame=239,groups=3,items=12,owned=12,buckets=0:1",
           "gs_queue": "mode=thread,policy=pipelined,frame_waits=0,frame_skip=1",
           "audio_ring": "pushed=1,dropped=0,pulled=1,underruns=0", "restore_mhash": "0x64d8f5db5dba9592"}
    lane_a = {k: "0xaaaa" for k in range(1, 601)}
    lane_seg = {k: "0xaaaa" for k in range(301, 600)}
    r = judge(log, meta_b, [(seg_dir, seg)], [], meta_a, 600, 900, 300, 600, lane_a, lane_seg)
    ok &= all(x for x, _ in r)
    # the relaunch route (the former F8): refused on the Relaunch line, the closed session, restores/
    log_old = (F8 + "Queued: Queued for the game owner.\n" + F8 + "Completed: Restarting from /o/slot0.ps2st (--load-state).\n" +
               F8 + "Relaunch: /b/nativeee --load-state /o/slot0.ps2st\n")
    r = judge(log_old, {"finalized": "1", "frames": "601", "park_detail": "session_closed"}, [], ["/o/leg_b/restores/1"],
              meta_a, 600, 900, 300, 600, lane_a, {})
    bad = [w for x, w in r if not x]
    ok &= any("Completed" in w for w in bad) and any("no Relaunch" in w for w in bad) and any("restores/" in w for w in bad) \
        and any("f8=live" in w for w in bad) and any("output-000001" in w for w in bad)
    # today's (pre-fix) live refusal: Refused, no segment
    r = judge(F8 + "Queued: x\n" + F8 + "Refused: A remaining asynchronous owner has no approved live-restore contract.\n",
              {"finalized": "1", "frames": "900", "f8": "live"}, [], [], meta_a, 600, 900, 300, 600, lane_a, {})
    ok &= sum(1 for x, _ in r if not x) >= 12
    # a restored session that crashed, or a digest that disagrees, or the audio ring dropping
    r = judge(log, meta_b, [(seg_dir, dict(seg, park="exception", park_detail="guest fault"))], [], meta_a, 600, 900, 300, 600, lane_a, lane_seg)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "park=none" in w]
    r = judge(log, meta_b, [(seg_dir, dict(seg, graphics_restore_unlinked="source_frame=0,groups=0,items=0"))], [], meta_a, 600, 900, 300, 600, lane_a, lane_seg)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "digest" in w]
    r = judge(log, meta_b, [(seg_dir, dict(seg, audio_ring="pushed=1,dropped=5,pulled=1,underruns=0"))], [], meta_a, 600, 900, 300, 600, lane_a, lane_seg)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "audio_ring" in w]
    # the first restored frame differs from the uninterrupted run's (and is not the no-image field)
    r = judge(log, meta_b, [(seg_dir, seg)], [], meta_a, 600, 900, 300, 600, lane_a, {**lane_seg, 301: "0xbbbb"})
    ok &= [w for x, w in r if not x] == [w for x, w in r if "first restored frame" in w]
    # a no-image first field is admitted when the uninterrupted picture follows within five keys
    r = judge(log, meta_b, [(seg_dir, seg)], [], meta_a, 600, 900, 300, 600, lane_a, {**lane_seg, 301: "0x0000000000000000"})
    ok &= all(x for x, _ in r)
    # ... but not when the picture never comes
    r = judge(log, meta_b, [(seg_dir, seg)], [], meta_a, 600, 900, 300, 600, lane_a,
              {k: "0x0000000000000000" if k < 310 else "0xaaaa" for k in range(301, 600)})
    ok &= [w for x, w in r if not x] == [w for x, w in r if "within the first five" in w]
    # the stale frame: an early restored frame equal to the pre-restore session's last picture
    r = judge(log, meta_b, [(seg_dir, seg)], [], meta_a, 600, 900, 300, 600, lane_a, {**lane_seg, 302: "0xstale"}, "0xstale")
    ok &= [w for x, w in r if not x] == [w for x, w in r if "pre-restore session's last picture" in w]
    # the restore landing at boundary 600 (the press's own boundary): frames=600 pre, 300 restored
    r = judge(log, dict(meta_b, frames="600"), [(seg_dir, dict(seg, frames="300"))], [], meta_a, 600, 900, 300, 600, lane_a, lane_seg)
    ok &= all(x for x, _ in r)
    r = judge(log, dict(meta_b, frames="600"), [(seg_dir, seg)], [], meta_a, 600, 900, 300, 600, lane_a, lane_seg)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "R: frames=" in w]
    # leg C: the process restore's mhash
    r = judge_process({"finalized": "1", "park": "none", "restore_mhash": "0x64d8f5db5dba9592"}, seg)
    ok &= all(x for x, _ in r)
    r = judge_process({"finalized": "1", "park": "none", "restore_mhash": "0x1"}, seg)
    ok &= [w for x, w in r if not x] == [w for x, w in r if "restore_mhash" in w]
    # agreement arithmetic
    ok &= agreement({1: "a", 2: "b", 3: "c"}, {1: "a", 2: "x", 4: "c", 3: "y"}, 1, 4) == (1, 1, 3)
    print("root_play_live_restore selftest: %s" % ("PASS" if ok else "FAIL"))
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
    ap.add_argument("--save-at", type=int, default=300, help="the F5 boundary (harness.save_at_frame)")
    ap.add_argument("--restore-at", type=int, default=600, help="the F8 boundary (harness.restore_at_frame)")
    ap.add_argument("--frames-a", type=int, default=600, help="leg A's cap (the uninterrupted run)")
    ap.add_argument("--frames-b", type=int, default=900, help="leg B's cap (the process's boundaries, both segments)")
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
    root = tempfile.mkdtemp(prefix="root_play_live_restore_")
    saves = os.path.join(root, "saves")
    leg_a, leg_b, leg_c = (os.path.join(root, d) for d in ("leg_a", "leg_b", "leg_c"))
    for d in (leg_a, leg_b, leg_c):
        os.makedirs(d)
    gf = os.path.abspath(args.game_files)
    common = ["--profile", "default", "--harness-window", "--game-files", gf, "--set", "paths.save_dir=" + saves]
    code_a = run(args, [args.binary] + common + ["--harness-out", leg_a, "--frames", str(args.frames_a),
                                                 "--set", "harness.save_at_frame=%d" % args.save_at],
                 os.path.join(root, "leg_a.log"), os.path.join(leg_a, "frames.fhash"))
    slot = os.path.join(saves, "savestates", "slot0.ps2st")
    meta_a = parse_meta(os.path.join(leg_a, "run.meta"))
    for line in open(os.path.join(root, "leg_a.log")).read().splitlines() if os.path.isfile(os.path.join(root, "leg_a.log")) else []:
        if line.startswith("nativeee: savestate"):
            print("stderr A: " + line[:300])
    if code_a == 0 and meta_a.get("finalized") == "1" and meta_a.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    if code_a != 0 or not os.path.isfile(slot):
        print("FAIL: leg A wrote no slot (runner exit %d; run.meta savestate=%s) -- kept at %s"
              % (code_a, meta_a.get("savestate", "<absent>"), root))
        return 1
    log_b = os.path.join(root, "leg_b.log")
    code_b = run(args, [args.binary] + common + ["--harness-out", leg_b, "--frames", str(args.frames_b),
                                                 "--set", "harness.restore_at_frame=%d" % args.restore_at],
                 log_b, os.path.join(leg_b, "frames.fhash"))
    log_text = open(log_b).read() if os.path.isfile(log_b) else ""
    for line in log_text.splitlines():
        if line.startswith("nativeee: savestate") or line.startswith("nativeee: --load-state") or line.startswith("nativeee: F8"):
            print("stderr B: " + line[:300])
    meta_b = parse_meta(os.path.join(leg_b, "run.meta"))
    segments = []
    for name in sorted(os.listdir(leg_b)) if os.path.isdir(leg_b) else []:
        if name.startswith("output-") and os.path.isfile(os.path.join(leg_b, name, "run.meta")):
            segments.append((os.path.join(leg_b, name), parse_meta(os.path.join(leg_b, name, "run.meta"))))
    restores = []
    rdir = os.path.join(leg_b, "restores")
    if os.path.isdir(rdir):
        restores = [os.path.join(rdir, s) for s in sorted(os.listdir(rdir))]
    for key in ("frames", "finalized", "f8", "output_next", "park", "park_detail"):
        print("run.meta B %s=%s" % (key, meta_b.get(key, "<absent>")))
    for d, m in segments:
        for key in ("frames", "finalized", "restore_route", "restored_vsync_frame", "output_restore_frame", "restore_mhash",
                    "park", "park_detail", "video", "graphics_live_restore", "graphics_restore_unlinked", "live_restore_refusals",
                    "frames_dropped", "mtvu", "wall_ms"):
            print("run.meta R(%s) %s=%s" % (os.path.basename(d), key, m.get(key, "<absent>")[:200]))
    if code_b == 0 and meta_b.get("finalized") == "1" and meta_b.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    lane_a = parse_lane(os.path.join(leg_a, "frames.fhash"))
    lane_seg = parse_lane(os.path.join(segments[0][0], "frames.fhash")) if segments else {}
    lane_b = parse_lane(os.path.join(leg_b, "frames.fhash"))
    lane_b_last = lane_b[max(lane_b)] if lane_b else None
    failures = 0
    for ok, what in judge(log_text, meta_b, segments, restores, meta_a, args.frames_a, args.frames_b, args.save_at,
                          args.restore_at, lane_a, lane_seg, lane_b_last):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if code_b != 0:
        print("  FAIL B: runner exit %d (the live-restored session must complete)" % code_b)
        failures += 1
    same, near, total = agreement(lane_seg, lane_a, args.save_at + 1, args.frames_a)
    print("  REPORT live restore vs uninterrupted frames %d..%d: same=%d near(+-3)=%d of %d"
          % (args.save_at + 1, args.frames_a - 1, same, near, total))
    # Leg C: the process restore of the same slot.
    tail = args.frames_a - args.save_at
    code_c = run(args, [args.binary] + common + ["--harness-out", leg_c, "--frames", str(tail), "--load-state", slot],
                 os.path.join(root, "leg_c.log"), os.path.join(leg_c, "frames.fhash"))
    meta_c = parse_meta(os.path.join(leg_c, "run.meta"))
    for key in ("frames", "finalized", "restored_vsync_frame", "restore_mhash", "park"):
        print("run.meta C %s=%s" % (key, meta_c.get(key, "<absent>")))
    seg_meta = segments[0][1] if segments else {}
    for ok, what in judge_process(meta_c, seg_meta):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if code_c != 0:
        print("  FAIL C: runner exit %d (the process restore must complete)" % code_c)
        failures += 1
    lane_c = parse_lane(os.path.join(leg_c, "frames.fhash"))
    same_c, near_c, total_c = agreement(lane_c, lane_a, args.save_at + 1, args.frames_a)
    print("  REPORT process restore vs uninterrupted frames %d..%d: same=%d near(+-3)=%d of %d"
          % (args.save_at + 1, args.frames_a - 1, same_c, near_c, total_c))
    if failures:
        print("RootPlayLiveRestore: FAIL (%d) -- legs kept at %s" % (failures, root))
        return 1
    if args.keep:
        print("legs kept at %s" % root)
    else:
        shutil.rmtree(root, ignore_errors=True)
    print("RootPlayLiveRestore: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
