#!/usr/bin/env python3
"""RootAnchorPadReady -- a READY controller stays READY across an anchor restore.

Runs the built `nativeee` binary as the harness does for an anchor leg
(verify/run_leg.py arming: `--profile deterministic --headless --harness-out
<dir> --frames N --state-every 1 --load-state <anchor> --game-files <dir>`,
plus `--set video.backend=soft` -- the mem lane is backend-independent and the
soft backend keeps the leg short), bounded through verify/run_bounded.sh, and
asserts on run.meta and on the boundary images the leg drops:

  * state_b0: both slots of port 0's pad area (the PADS section's area) carry
    the READY image -- +0x70 state 0x06 STABLE, +0x60 length 0x20, +0x01 mode
    0x79 -- not the zeroed DISCONN slot (state 0x00, reqState 0x02) that a
    pad protocol re-running its connection sequence from the restored boundary
    clock (restarting at 0) writes
  * state_b2 / state_b3: the title's controller state machine (FUN_00177168,
    object at *0x309508, case = (word(obj+4) >> 12) & 0x3f) is still in its
    gameplay case 12 -- not the controller-disconnected hold, case 15, that
    FUN_00176698 enters when the slot reads disconnected
  * run.meta pad_stage=2,0 (port 0 READY), restore_mhash_ok=1, finalized=1,
    frames=N
  * state_b0: the slot sequence counters (+0x58) continue from the anchor's
    (anchor+1 / anchor+2): PADMAN's frame counter is in the restored pad area,
    and the PADS decode derives the protocol's counter from it; b2 therefore
    reads anchor+3 (the original wrote 5 there: its process-static counter
    restarted at 1 after every load: the pad sequence is not saved)

A real process run, not a self-report: with PadProtocol::Stamp recomputing
the stage from the restarted clock the b0 slots read state 0x00 and the title
holds in case 15 from boundary 2 ('s INT-I85G-11K-CLASS-IS-PAD-RESTORE).
Exit 77 (SKIP) when the title files or the anchor are absent.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
CTRL_OBJECT_PTR = 0x00309508   # TitleProfile::Pad ctrl_object_ptr
CASE_GAMEPLAY = 12
CASE_DISCONNECT_HOLD = 15
SLOT_BYTES = 0x80
SEQ_OFFSET = 0x58      # PadProtocol::kSeqOffset: the slot sequence counter


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


def pads_area(state_hash, sections, port):
    """The PADS section's pad area for `port` (pads_section.h wire layout: u32 area[2] first)."""
    tag = state_hash.tag("PADS")
    if tag not in sections:
        return None
    payload = sections[tag][2]
    return struct.unpack_from("<II", payload, 0)[port]


def slot_image(ram, slot):
    return {"mode": ram[slot + 0x01], "len": ram[slot + 0x60], "state": ram[slot + 0x70],
            "req": ram[slot + 0x71], "task": ram[slot + 0x72]}


def ctrl_case(ram):
    obj = struct.unpack_from("<I", ram, CTRL_OBJECT_PTR)[0]
    if not (0x10000 <= obj < len(ram) - 8):
        return obj, None
    w4 = struct.unpack_from("<I", ram, obj + 4)[0]
    return obj, (w4 >> 12) & 0x3F


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--verify-dir", required=True, help="the repo's verify/ (state_hash.py)")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.anchor):
        print("SKIP: anchor absent: %s" % args.anchor)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    sys.path.insert(0, os.path.abspath(args.verify_dir))
    import state_hash  # noqa: E402

    out_dir = tempfile.mkdtemp(prefix="root_anchor_pad_ready_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames), "--state-every", "1",
           "--load-state", os.path.abspath(args.anchor),
           "--game-files", os.path.abspath(args.game_files),
           "--set", "video.backend=soft",
           # the Soft backend hosts no selected Graphics producer: the pre-flip
           # control arm is stated explicitly (cutover flip 1 made sprites the default;
           # cutover flip 2 made the owned frame environment the default;
           # cutover flip 3 made native Strip primitives the default -- all three reverted)
           "--set", "hle.graphics_sprites=off",
           "--set", "hle.graphics_frame=off",
           "--set", "hle.graphics_strips=plain",
           # cutover flip 7 made the owned Movie pictures the default (a GL-only producer)
           "--set", "hle.graphics_movie=off",
           # ALL-ON build (2026-10-09): the Soft backend admits no Graphics producer and
           # the player chain requires graphics_movie -- the retained arms are stated
           # explicitly (default-independent; nothing weakened).
           "--set", "hle.graphics_text=off", "--set", "hle.graphics_sky=off",
           "--set", "hle.graphics_shadows=off", "--set", "hle.graphics_distortion=off",
           "--set", "hle.movie_step=off", "--set", "hle.movie_start=off",
           "--set", "hle.movie_wait=off", "--set", "hle.movie_stop=off"]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("pad_stage", "pad_calls", "pad_readiness", "restore_mhash_ok", "finalized", "frames", "park"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)
    check(meta.get("pad_stage", "").startswith("2,"), "pad_stage port 0 = 2 (READY kept after the restore)")

    # The anchor itself: port 0 restored READY with a STABLE slot image.
    _, _, anchor_sections = state_hash.parse(args.anchor)
    area = pads_area(state_hash, anchor_sections, 0)
    print("anchor PADS port0 area=%s" % ("0x%x" % area if area else "<none>"))
    check(bool(area), "the anchor's PADS section names port 0's pad area")

    b0 = os.path.join(out_dir, "state_b0.ps2st")
    anchor_seq = None
    if area:
        anchor_ram = anchor_sections[state_hash.TAG_RAM][2]
        anchor_seq = struct.unpack_from("<I", anchor_ram, area + SEQ_OFFSET)[0]
        print("anchor slot0 seq @0x%x = %d (slot1 %d)" % (
            area + SEQ_OFFSET, anchor_seq, struct.unpack_from("<I", anchor_ram, area + SLOT_BYTES + SEQ_OFFSET)[0]))
    if area and os.path.isfile(b0):
        _, _, s = state_hash.parse(b0)
        ram = s[state_hash.TAG_RAM][2]
        for n in range(2):
            img = slot_image(ram, area + n * SLOT_BYTES)
            print("state_b0 port0 slot%d @0x%x: mode=0x%02x len=0x%02x state=0x%02x req=0x%02x task=0x%02x"
                  % (n, area + n * SLOT_BYTES, img["mode"], img["len"], img["state"], img["req"], img["task"]))
            check(img["state"] == 0x06 and img["len"] == 0x20 and img["mode"] == 0x79 and img["req"] == 0x00,
                  "state_b0 slot%d is the READY image (state 0x06 STABLE, len 0x20, mode 0x79, reqState 0)" % n)
        # The slot sequence counter continues from the anchor's: one stamp per
        # boundary, so b0 carries anchor + 1 (slot 1 anchor + 2) -- not the boot
        # counter's 2 / 3 that a protocol restarting its counter at 1 writes.
        seq0 = struct.unpack_from("<I", ram, area + SEQ_OFFSET)[0]
        seq1 = struct.unpack_from("<I", ram, area + SLOT_BYTES + SEQ_OFFSET)[0]
        print("state_b0 seq slot0=%d slot1=%d (anchor %d)" % (seq0, seq1, anchor_seq))
        check(seq0 == anchor_seq + 1 and seq1 == anchor_seq + 2,
              "state_b0 seq words continue from the anchor's counter (anchor+1 / anchor+2)")
    else:
        check(False, "state_b0.ps2st written (--state-every 1)")

    for n in (2, 3):
        if n >= args.frames:
            continue
        path = os.path.join(out_dir, "state_b%d.ps2st" % n)
        if not os.path.isfile(path):
            check(False, "state_b%d.ps2st written" % n)
            continue
        _, _, s = state_hash.parse(path)
        ram = s[state_hash.TAG_RAM][2]
        obj, case = ctrl_case(ram)
        print("state_b%d ctrl_obj=0x%x case=%s" % (n, obj, case))
        check(case == CASE_GAMEPLAY,
              "state_b%d controller SM case is %d (gameplay), not %d (controller-disconnected hold)"
              % (n, CASE_GAMEPLAY, CASE_DISCONNECT_HOLD))
        if area and anchor_seq is not None:
            seq0 = struct.unpack_from("<I", ram, area + SEQ_OFFSET)[0]
            print("state_b%d seq slot0 @0x%x = %d (anchor %d + %d)" % (n, area + SEQ_OFFSET, seq0, anchor_seq, n + 1))
            check(seq0 == anchor_seq + n + 1, "state_b%d seq word is anchor+%d (one stamp per boundary)" % (n, n + 1))

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootAnchorPadReady: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
