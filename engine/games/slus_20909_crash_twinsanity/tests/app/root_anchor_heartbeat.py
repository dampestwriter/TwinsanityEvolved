#!/usr/bin/env python3
"""RootAnchorHeartbeat -- the restored STREAM.IRX heartbeat beats after an anchor restore.

Runs the built `nativeee` binary as the harness does for an anchor leg
(`--profile deterministic --headless --harness-out <dir> --frames N --state-every 1
--load-state <anchor> --game-files <dir> --set video.backend=soft`), bounded through
verify/run_bounded.sh, and asserts on run.meta and on the boundary images:

  * run.meta iop_timer_running lists hard timer 1 (the heartbeat re-armed by the
    plugin) AND iop_hb_beats >= frames - 1 (the kernel's beat census: the timer's
    due, one period past the restored now, is reached by the boundary pumps once the
    FRAME clock's frame base is the anchor's frame count -- IopClock::RebaseToFrame
    in IopmSection::ReconcileKernel); iop_clock=frames=<restored_vf + N>,us=...
  * state_b1: the module's voice-record type byte of the probed voice (IOP RAM
    0x12f60 + v, one byte per voice; 4 = finished-not-yet-released, 1 = released,
    0 = free) reads 1 -- the release pass (iop_func_0000a1e0) ran, which only the
    heartbeat handler's tick word (0x12f1c) lets the worker enter; no record still
    reads 4
  * state_b1: every voice the anchor records finished (type byte 4, parked at the
    silence block 0x5000) is Off (phase 0) in the SNDC section's voice model -- the
    module's sweep (iop_func_0000a2ac) issued the KOFF the frozen clock withheld
    (41 voices on slowspot_f40298; OLD's `[KSWQ] vf=40304/40305 KOFF` masks)
  * the probed voice (the one still PLAYING at the save with its ENDX latch set,
    v29 on slowspot_f40298): its type byte reads 1 at state_b1 (released, awaiting
    the sweep's KOFF); its model phase at state_b2 is PRINTED, not asserted -- the
    sweep keys it off only when the ENDX latch carries its bit, and the worker's
    RPC-woken sweep of boundary 0 clears the restored latch (SdSetSwitch ENDX 0)
    before the first beat's release pass, so the bit re-latches only when the voice
    reaches its end block again ('s INT-I88A-RESTORED-ENDX-CLEARED-BEFORE-FIRST-BEAT)

  * `--restored-timer-due at_restore` (RootAnchorHeartbeatAtRestore): the leg
    runs with `--set iop.restored_timer_due=at_restore` -- the restored heartbeat is due
    AT the restore, so its first beat (release pass) runs in the first IOP pump of
    boundary 0 BEFORE the EE's RPC wakes the worker, the sweep of boundary 0 sees the
    restored ENDX latch intact and keys the probed voice off: run.meta
    iop_restored_timer_due=at_restore, iop_hb_beats >= frames (one extra beat), and
    the dropped check is asserted again -- the probed voice's type byte reads 1
    and its SNDC phase Off (0) already at state_b0, Off at state_b1 with the type byte 0 or
    1 (released at the restore, freed by the next pass once silent) -- OLD's `[KSWQ]
    vf=40304 KOFF core=0 mask=0xfffffc` order. Under the default (one_period) the phase is
    printed only, as before.

A real process run, not a self-report: with the restore rebasing only the clock's
microsecond base the leg reports iop_hb_beats=0, every type byte keeps its anchor
value (42 x 4 on slowspot_f40298) and every finished voice stays Sustain (the
audio W0 class is a dead heartbeat). Exit 77 (SKIP) only when the title files or
the anchor are absent.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
TYPE_BYTES = 0x12F60        # STREAM.IRX voice records: one type byte per voice
TICK_WORD = 0x12F1C         # the heartbeat handler's tick word
NVOICES = 48
SILENCE_BLOCK = 0x5000      # libsd's silence block: a finished voice parks its NAX here
VS_FMT = "<ffHIIIBfffffii28hBBiIh"   # engine/sound/sound_state_codec.h VoiceState (119 B)
VS_SIZE = struct.calcsize(VS_FMT)
PHASES = {0: "Off", 1: "Attack", 2: "Decay", 3: "Sustain", 4: "Release"}


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


def iopm_ram(state_hash, sections):
    """The IOPM section's IOP RAM (iopm_section.h wire layout: u32 ram_size, u8[ram_size] ...)."""
    tag = state_hash.tag("IOPM")
    if tag not in sections:
        return None
    payload = sections[tag][2]
    (size,) = struct.unpack_from("<I", payload, 0)
    return payload[4:4 + size]


def sndc_voices(state_hash, sections):
    """The SNDC section's 48 VoiceState records (sound_state_codec.h: u32 spu_ram_size,
    u8[spu_ram_size], u32 nvoices, nvoices x VoiceState ...)."""
    tag = state_hash.tag("SNDC")
    if tag not in sections:
        return None
    payload = sections[tag][2]
    off = 0
    (spu_size,) = struct.unpack_from("<I", payload, off)
    off += 4 + spu_size
    (nv,) = struct.unpack_from("<I", payload, off)
    off += 4
    voices = []
    for _ in range(nv):
        voices.append(struct.unpack_from(VS_FMT, payload, off))
        off += VS_SIZE
    return voices


def voice_line(v):
    (vol_l, vol_r, pitch, ssa, lsax, nax, phase, env) = v[:8]
    return "ph=%s env=%.6f pitch=0x%04x ssa=0x%06x lsax=0x%06x nax=0x%06x" % (
        PHASES.get(phase, str(phase)), env, pitch, ssa, lsax, nax)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--verify-dir", required=True, help="the repo's verify/ (state_hash.py)")
    ap.add_argument("--voice", type=int, default=29, help="the voice the anchor records playing that the module's sweep kills")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--restored-timer-due", choices=("one_period", "at_restore"), default="one_period",
                    help="Config iop.restored_timer_due for the leg (at_restore asserts the probed voice Off by state_b1)")
    args = ap.parse_args()
    at_restore = args.restored_timer_due == "at_restore"

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

    out_dir = tempfile.mkdtemp(prefix="root_anchor_heartbeat_")
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
    if at_restore:
        cmd += ["--set", "iop.restored_timer_due=at_restore"]
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
    for key in ("iop_timer_running", "iop_hb_beats", "iop_restored_timer_due", "iop_clock", "iopm_restored",
                "restore_mhash_ok", "finalized", "frames", "park", "snd_active"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)
    check(meta.get("iopm_restored") == "1", "iopm_restored=1")
    check("1" in meta.get("iop_timer_running", "").split(","), "iop_timer_running lists hard timer 1 (the heartbeat)")
    try:
        beats = int(meta.get("iop_hb_beats", "-1"))
    except ValueError:
        beats = -1
    check(beats >= args.frames - 1, "iop_hb_beats=%d >= %d (about one heartbeat beat per boundary pump)" % (beats, args.frames - 1))
    check(meta.get("iop_restored_timer_due") == args.restored_timer_due,
          "iop_restored_timer_due=%s (the leg's Config toggle)" % args.restored_timer_due)
    if at_restore:
        check(beats >= args.frames, "iop_hb_beats=%d >= %d (at_restore: the extra beat at the restore itself)" % (beats, args.frames))

    anchor_ver, anchor_vf, anchor_sections = state_hash.parse(args.anchor)
    print("anchor vsync_frame=%d" % anchor_vf)
    clock = dict(kv.split("=", 1) for kv in meta.get("iop_clock", "").split(",") if "=" in kv)
    check(clock.get("frames") == str(anchor_vf + args.frames),
          "iop_clock frames=%s == anchor %d + %d boundaries (the FRAME clock's base is the anchor's frame count)"
          % (clock.get("frames"), anchor_vf, args.frames))

    anchor_ram = iopm_ram(state_hash, anchor_sections)
    if anchor_ram is None:
        check(False, "the anchor carries an IOPM section")
        anchor_types = b""
    else:
        anchor_types = anchor_ram[TYPE_BYTES:TYPE_BYTES + NVOICES]
        print("anchor type bytes 0x%x: %s counts=%s" % (
            TYPE_BYTES, anchor_types.hex(), dict(sorted(Counter(anchor_types).items()))))
        check(anchor_types[args.voice] in (1, 4), "the anchor records voice v%d non-free (type %d)" % (args.voice, anchor_types[args.voice]))
    anchor_voices = sndc_voices(state_hash, anchor_sections)
    finished = []   # the voices the module's release pass + sweep must key off
    if anchor_voices:
        print("anchor v%02d %s" % (args.voice, voice_line(anchor_voices[args.voice])))
        check(anchor_voices[args.voice][6] != 0, "the anchor's voice model has v%d playing (phase != Off)" % args.voice)
        for vi in range(NVOICES):
            if vi < len(anchor_types) and anchor_types[vi] == 4 and anchor_voices[vi][5] == SILENCE_BLOCK and anchor_voices[vi][6] != 0:
                finished.append(vi)
        print("anchor finished-but-keyed voices (type 4, nax=0x5000, phase != Off): %d -> %s" % (len(finished), finished))
        check(len(finished) >= 1, "the anchor records finished voices awaiting the module's release")

    for n in range(min(args.frames, 3)):
        path = os.path.join(out_dir, "state_b%d.ps2st" % n)
        if not os.path.isfile(path):
            check(False, "state_b%d.ps2st written (--state-every 1)" % n)
            continue
        _, _, s = state_hash.parse(path)
        ram = iopm_ram(state_hash, s)
        voices = sndc_voices(state_hash, s)
        types = ram[TYPE_BYTES:TYPE_BYTES + NVOICES] if ram is not None else b""
        tick = struct.unpack_from("<I", ram, TICK_WORD)[0] if ram is not None else -1
        print("state_b%d type bytes: %s counts=%s tick=0x%x" % (
            n, types.hex(), dict(sorted(Counter(types).items())), tick))
        if voices:
            print("state_b%d v%02d %s" % (n, args.voice, voice_line(voices[args.voice])))
        if n == 0 and at_restore:
            check(len(types) == NVOICES and types[args.voice] == 1,
                  "state_b0 v%d type byte reads 1 (at_restore: the first beat's release pass ran in boundary 0)" % args.voice)
            check(bool(voices) and voices[args.voice][6] == 0,
                  "state_b0 v%d phase Off (at_restore: keyed off within pump 0 -- OLD's `[KSWQ] vf=40304 KOFF core=0` order)" % args.voice)
        if n == 1:
            if at_restore:
                # Released by the first beat (type 1), then freed by the next release pass once its
                # envelope read 0 (type 0, the record's word cleared): one beat earlier than one_period.
                check(len(types) == NVOICES and types[args.voice] in (0, 1),
                      "state_b1 v%d type byte reads 0 or 1 (at_restore: released at the restore, freed once silent)" % args.voice)
            else:
                check(len(types) == NVOICES and types[args.voice] == 1,
                      "state_b1 v%d type byte reads 1 (released by the heartbeat-gated release pass)" % args.voice)
            check(len(types) == NVOICES and 4 not in types,
                  "state_b1 no voice record still reads type 4 (the release pass ran)")
            still_on = [vi for vi in finished if voices and voices[vi][6] != 0]
            check(bool(voices) and finished and not still_on,
                  "state_b1 every finished voice of the anchor is Off in the voice model (%d keyed off; still on: %s)"
                  % (len(finished) - len(still_on), still_on))
            if at_restore and voices:
                check(voices[args.voice][6] == 0,
                      "state_b1 v%d phase Off (at_restore: the first beat's release pass precedes the RPC-woken sweep, "
                      "so the sweep's KOFF carries the restored ENDX bit)" % args.voice)
        if n == 2 and voices:
            print("state_b2 v%02d phase=%s (reported, not asserted: see the docstring)"
                  % (args.voice, PHASES.get(voices[args.voice][6], str(voices[args.voice][6]))))

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootAnchorHeartbeat: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
