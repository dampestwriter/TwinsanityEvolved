#!/usr/bin/env python3
"""RootFramesKeyConvention / RootDiffLegsCoalesced -- the frames lane's pairing
decisions (user decision 2026-10-01) hold on real legs of the built
`nativeee` binary.

  --mode anchor_keys   a short deterministic leg restored from the artdet anchor
                       (OLD save_states, read-only; SKIP 77 when absent) writes
                       frames.fhash keyed by the original's vsync-frame convention:
                       key 1 for its first present, then restored_vsync_frame + 1,
                       + 2, ... -- the SAME keys as the frozen baseline's first lines
                       (games/slus_20909_crash_twinsanity/verify/baseline/artdet_f37152/frames.fhash: 1, 37157, 37158).
  --mode coalesced     a coldboot leg long enough to pass the baseline's first
                       coalesced presents (keys 64/65) compared with
                       games/slus_20909_crash_twinsanity/verify/baseline/coldboot through diff_legs --cross-engine
                       --lanes frames: the report lists the coalesced keys and the
                       verdict line carries `coalesced 2` -- never silently (the
                       verdict WORD is not asserted here: the hash values are the
                       Phase-4/5 gates' business).
  --mode gs_restored the same 3-frame
                       artdet leg restores the GS-owned sections: run.meta
                       gs_sections_restored=1/1/1 (VRAM / GSPR / GSST through
                       GsSections), gs_restored_dispfb1=0x11000 (the anchor's
                       DISPFB1: FBP 0, FBW 8, PSMCT16), the key-1 present -- the
                       restore point's scanout -- is NOT the 512x448 black frame
                       (0xa741b33a5c037124 = FrameHasher::HashNativeScale of
                       all-zero RGB) that a root registering no GS provider
                       presents, and the key-1 grab read the TITLE'S DISPLAY
                       BUFFER TARGET at the scanout base (census
                       gs_key1_grab=target@0x0, no most-recently-drawn fallback):
                       the restored title draws its display copy into DISPFB (FBP
                       0) before the first vblank, so the console scans out that
                       copy, not the anchor's saved DISPFB bytes (BUG-076 OPEN
                       part re-attributed). The anchor's DISPFB decode
                       (anchor_dispfb_decode.py) is printed for the record; it is
                       NOT asserted equal -- the copy's source pages (FRAME
                       0x70000) are stale in every OLD anchor (row candidate
                       P5-ANCHORS-STALE-RT-VRAM).

Never SKIPs for a leg that started: a crash or stall is a failure.
"""
import argparse
import os
import subprocess
import sys
import tempfile

# anchor_dispfb_decode is the engine's (tests/app, title-neutral); this script is the title's.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "tests", "app"))
import anchor_dispfb_decode  # noqa: E402

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
# FrameHasher::HashNativeScale (engine/core/verify/frame_hasher.cpp) over a 512x448
# all-zero RGB frame: what a restored leg presents at key 1 when the root restores
# no GS section (VRAM empty, DISPFB at reset) -- anchor_dispfb_decode.hash_rgb_topdown
# (512, 448, bytes(512 * 448 * 3)) reproduces it; equal to S6's NEW key 1 on all 11
# anchors.
BLACK_512X448_FHASH = 0xA741B33A5C037124
ARTDET_DISPFB1 = "0x11000"   # FBP 0, FBW 8, PSM PSMCT16 (the anchors' GSPR)


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    return meta


def read_keys(path):
    keys = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                keys.append(int(line.split(",", 1)[0]))
    return keys


def read_hashes(path):
    """frames.fhash lines as (key, hash) pairs."""
    pairs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                k, h = line.split(",", 1)
                pairs.append((int(k), int(h, 16)))
    return pairs


def run_leg(args, tag, extra):
    out_dir = tempfile.mkdtemp(prefix="root_frames_pairing_%s_" % tag)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)] + extra
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run[%s]: %s" % (tag, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "60"
    proc = subprocess.run(cmd, env=env)
    print("runner[%s] exit=%d" % (tag, proc.returncode))
    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "restored_vsync_frame", "presents", "wall_ms",
                "gs_sections_restored", "gs_restored_dispfb1", "gs_key1_grab"):
        print("  %s=%s" % (key, meta.get(key, "<absent>")))
    return out_dir, meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--repo", required=True, help="the repo root (verify/, games/slus_20909_crash_twinsanity/verify/baseline/)")
    ap.add_argument("--mode", choices=("anchor_keys", "coalesced", "gs_restored"), required=True)
    ap.add_argument("--anchor", default="", help="artdet_f37152.ps2st (anchor_keys / gs_restored modes)")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection forwarded to the binary (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    failures = []

    def check(cond, what):
        print("  [%s] %s" % ("ok" if cond else "FAIL", what))
        if not cond:
            failures.append(what)

    if args.mode == "anchor_keys":
        if not os.path.isfile(args.anchor):
            print("SKIP: anchor absent: %s" % args.anchor)
            return 77
        baseline = os.path.join(args.repo, "games", "slus_20909_crash_twinsanity", "verify", "baseline", "artdet_f37152", "frames.fhash")
        out_dir, meta = run_leg(args, "anchor", ["--load-state", os.path.abspath(args.anchor)])
        check(meta.get("finalized") == "1", "leg finalized")
        check(meta.get("frames") == str(args.frames), "frames=%d" % args.frames)
        rvf = int(meta.get("restored_vsync_frame", "0") or 0)
        check(rvf > 0, "restored_vsync_frame read from the anchor (%d)" % rvf)
        keys = read_keys(os.path.join(out_dir, "frames.fhash"))
        expected = [1] + [rvf + i for i in range(1, args.frames)]
        print("  keys NEW      : %s" % keys)
        print("  keys expected : %s" % expected)
        check(keys == expected, "keys = 1, restored_vsync_frame+1, ... (the original's vsync-frame convention)")
        base_keys = read_keys(baseline)[:args.frames]
        print("  keys baseline : %s" % base_keys)
        check(keys == base_keys, "keys equal the frozen baseline's first %d keys" % args.frames)
    elif args.mode == "gs_restored":
        if not os.path.isfile(args.anchor):
            print("SKIP: anchor absent: %s" % args.anchor)
            return 77
        out_dir, meta = run_leg(args, "gs_restored", ["--load-state", os.path.abspath(args.anchor)])
        check(meta.get("finalized") == "1", "leg finalized")
        check(meta.get("frames") == str(args.frames), "frames=%d" % args.frames)
        # The root registers GsSections: all three GS-owned sections restored (VRAM
        # / GSPR / GSST). Before the registration the key was absent from run.meta.
        check(meta.get("gs_sections_restored") == "1/1/1",
              "gs_sections_restored=1/1/1 (VRAM / GSPR / GSST restored through GsSections)")
        check(meta.get("gs_restored_dispfb1", "").lower() == ARTDET_DISPFB1,
              "gs_restored_dispfb1=%s (the anchor's DISPFB1, not the reset value)" % ARTDET_DISPFB1)
        pairs = read_hashes(os.path.join(out_dir, "frames.fhash"))
        print("  frames.fhash  : %s" % ["%d,0x%016x" % p for p in pairs[:3]])
        check(len(pairs) >= 1 and pairs[0][0] == 1, "line 1 is keyed 1 (the restore-point present)")
        check(len(pairs) >= 1 and pairs[0][1] != BLACK_512X448_FHASH,
              "key-1 hash != 0x%016x (not the 512x448 black frame of an unrestored GS)" % BLACK_512X448_FHASH)
        check(len(pairs) >= 1 and pairs[0][1] != 0,
              "key-1 hash != 0 (a present with a readable scanout)")
        # The key-1 grab is the title's display buffer target at the scanout base
        # (the title's display copy, drawn before the first vblank) -- never the
        # most-recently-drawn fallback, never a render target elsewhere.
        check(meta.get("gs_key1_grab") == "target@0x0",
              "gs_key1_grab=target@0x0 (boundary 0's grab read the title's display buffer target at DISPFB, no fallback)")
        dec = anchor_dispfb_decode.dispfb_decode(os.path.abspath(args.anchor), args.repo)
        check(dec is not None, "the anchor's DISPFB1 decodes (PSMCT16 display out of the VRAM section; for the record)")
        if dec is not None:
            print("  anchor DISPFB1: FBP=%d FBW=%d px PSM=%d -> %dx%d fhash=0x%016x (saved bytes; key 1 %s them: "
                  "the title's copy overwrites DISPFB before the first vblank)"
                  % (dec["fbp"], dec["fbw_px"], dec["psm"], dec["w"], dec["h"], dec["fhash"],
                     "equals" if pairs and pairs[0][1] == dec["fhash"] else "differs from"))
    else:
        baseline = os.path.join(args.repo, "games", "slus_20909_crash_twinsanity", "verify", "baseline", "coldboot")
        out_dir, meta = run_leg(args, "coldboot", [])
        check(meta.get("finalized") == "1", "leg finalized")
        keys = read_keys(os.path.join(out_dir, "frames.fhash"))
        check(keys == list(range(1, args.frames + 1)), "coldboot keys 1..%d, one line per boundary" % args.frames)
        cmd = [sys.executable, os.path.join(args.repo, "verify", "diff_legs.py"), baseline, out_dir,
               "--cross-engine", "--lanes", "frames"]
        print("diff: %s" % " ".join(cmd))
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        print(proc.stdout)
        lines = proc.stdout.splitlines()
        check(proc.returncode in (0, 1), "diff_legs ran the frames lane (exit %d, not a usage/meta error)" % proc.returncode)
        check(any(l.startswith("frames.fhash: coalesced presents (keys the A leg lacks") and " -- 64,65" in l for l in lines),
              "the coalesced presents 64,65 (the original's VSync coalesce skip) are listed")
        check(any(l.startswith("diff_legs: ") and l.endswith(", coalesced 2)") for l in lines),
              "the verdict line carries `coalesced 2`")
    print("RESULT: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
