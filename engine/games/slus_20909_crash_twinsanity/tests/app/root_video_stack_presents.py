#!/usr/bin/env python3
"""RootVideoStackPresents -- the composition root builds the live video stack and
feeds the DrawIR stream into DrawPipeline, with the real present / grab / hash
into frames.fhash (INTEGRATION_PLAN items 6.3 / 6.5).

Runs the built `nativeee` binary exactly as the harness does for an anchor leg
(verify/run_leg.py: `--profile deterministic --headless --harness-out <dir>
--frames N --load-state <anchor> --game-files <dir>`), bounded through
verify/run_bounded.sh, and asserts on the produced run.meta and frames.fhash:

  * video=gl                              -- a GL context was created headless
  * gl_renderer is set and is not "none"  -- the present reported its renderer
  * presents > 0                          -- OnFrameEnd presented (a DISPFB job
                                             set the scanout base)
  * drawir frames=N, gs_frame_boundaries=N -- the VSync job closed every frame
  * gfx_consumer draws>0,uploads_dropped=0 -- the consumer lowered the stream
  * frames.fhash has N lines `<vf>,0x<16 hex>` and NONE is the black-frame hash
    (the Phase-3 stub's 640x448 all-zero frame) -- the hash comes from the grab
  * finalized=1, frames=N
  * gfx_vram_epoch == gs_vram_epoch > 0     -- the pipeline's decode tiers read
                                             the GS's own VRAM image (one local
                                             memory), not a second image the GS
                                             never writes

When the leg reports video=stub (no GL context on this machine) the test SKIPs
(exit 77) after checking the stub still produced the leg: the harness contract
says a leg without GL runs with the stub present and says so. Exit 77 as well
when the title files or the anchor are absent.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
FHASH_LINE = re.compile(r"^(\d+),0x([0-9a-f]{16})$")


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def black_frame_hash(w=640, h=448):
    # FrameHasher::HashNativeScale: u32 nw LE, u32 nh LE, then R,G,B per pixel top-down.
    head = w.to_bytes(4, "little") + h.to_bytes(4, "little")
    return fnv1a64(head + bytes(w * h * 3))


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


def parse_fields(value):
    out = {}
    for part in value.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def as_int(text):
    try:
        return int(text)
    except (TypeError, ValueError):
        return -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
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

    out_dir = tempfile.mkdtemp(prefix="root_video_stack_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--load-state", os.path.abspath(args.anchor),
           "--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0
    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("video", "gl_renderer", "presents", "grab_empty", "gfx_consumer", "gs_frame_boundaries",
                "drawir", "finalized", "frames", "gs_vram_epoch", "gfx_vram_epoch"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    lines = []
    fh = os.path.join(out_dir, "frames.fhash")
    if os.path.isfile(fh):
        with open(fh, "r", encoding="utf-8", errors="replace") as f:
            lines = [l.strip() for l in f if l.strip()]
    for l in lines[:3]:
        print("frames.fhash %s" % l)

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)
    check(len(lines) == args.frames and all(FHASH_LINE.match(l) for l in lines),
          "frames.fhash has %d `<vf>,0x<16 hex>` lines" % args.frames)
    check(meta.get("gs_frame_boundaries") == str(args.frames), "gs_frame_boundaries=%d (the VSync job per boundary)" % args.frames)
    drawir = parse_fields(meta.get("drawir", ""))
    check(drawir.get("frames") == str(args.frames), "drawir frames=%d (the counting sink saw every VSync job)" % args.frames)

    if meta.get("video") == "stub":
        if failures:
            print("artifacts kept: %s %s" % (out_dir, log))
            print("RootVideoStackPresents: FAIL (stub leg broken)")
            return 1
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
        print("SKIP: video=stub (no GL context on this machine); the stub leg is intact")
        return 77

    black = "0x%016x" % black_frame_hash()
    check(meta.get("video") == "gl", "video=gl (the live stack was built headless)")
    check(meta.get("gl_renderer", "none") not in ("", "none"), "gl_renderer is set (%s)" % meta.get("gl_renderer"))
    check(as_int(meta.get("presents")) > 0, "presents > 0 (OnFrameEnd presented after the first DISPFB job)")
    gfx = parse_fields(meta.get("gfx_consumer", ""))
    check(as_int(gfx.get("draws")) > 0, "gfx_consumer draws > 0 (the consumer lowered the draws)")
    check(gfx.get("uploads_dropped") == "0", "gfx_consumer uploads_dropped=0 (every upload payload arrived)")
    check(gfx.get("frames") == str(args.frames), "gfx_consumer frames=%d" % args.frames)
    hashes = [l.split(",")[1] for l in lines if FHASH_LINE.match(l)]
    check(hashes and all(h != black for h in hashes), "no frames.fhash line is the black-frame hash (%s)" % black)
    check(hashes and all(h != "0x0000000000000000" for h in hashes), "no frames.fhash line is the no-target hash 0")
    check(as_int(meta.get("gs_vram_epoch")) > 0, "gs_vram_epoch > 0 (the GS uploaded into its VRAM image)")
    check(meta.get("gfx_vram_epoch") == meta.get("gs_vram_epoch"),
          "gfx_vram_epoch == gs_vram_epoch (the pipeline reads the GS's image: %s vs %s)"
          % (meta.get("gfx_vram_epoch"), meta.get("gs_vram_epoch")))

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootVideoStackPresents: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
