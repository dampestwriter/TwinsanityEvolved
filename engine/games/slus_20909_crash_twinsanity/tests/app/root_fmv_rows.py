#!/usr/bin/env python3
"""RootBindsFmvRows -- the composition root constructs the IPU device, the sceMpeg
HLE and the libmpeg / libipu manifest rows (INTEGRATION_PLAN items 7.3 / 7.4).

Runs the built `nativeee` binary exactly as the harness does (a deterministic
headless coldboot leg), bounded through verify/run_bounded.sh, and asserts on the
produced run.meta:

  * libmpeg_rows=15, libipu_rows=4    -- LibmpegBindings::Bind / LibipuBindings::Bind ran
  * avcodec_version is set            -- the FFmpeg decoder is linked (checklist item 15's key)
  * fmv= carries inits=1              -- sceMpegInit reached MpegApi::Init through its row
  * libipu_calls= carries init=1      -- sceMpegInit's sceIpuInit tail reached LibipuApi
  * movie_decoder served every picture with no fallback or IPU RestartDMA call
    -- the title picture boundary, Create and Reset execute their software path
  * unhandled_mmio=0                  -- the IPU window is on the router (item 7.4)
  * park_detail is not `libipu.a sceIpuInit` nor a `libcdvd.a sceCdSt*` park -- the
    leg advances past the first FMV's init (frames=N, the --frames cap)
  * finalized=1

With --require-subsystem-census, also require the DMA/IPU/IOP observation
fields, no observed unsupported operations, and served movie pictures.
Exit 77 (SKIP) when the title files are absent.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection (e.g. hle.movie_lifecycle=off: the retained arm this pin measures)")
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
    ap.add_argument("--require-subsystem-census", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_fmv_rows_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
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
    for key in ("libmpeg_rows", "libipu_rows", "avcodec_version", "fmv", "libipu_calls", "unhandled_mmio",
                "game_libraries_installed", "movie_decoder", "math_library", "sound_bank_headers",
                "park", "park_detail", "finalized", "frames", "fmv_pictures_served"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("libmpeg_rows") == "15", "libmpeg_rows=15 (LibmpegBindings::Bind)")
    check(meta.get("libipu_rows") == "4", "libipu_rows=4 (LibipuBindings::Bind)")
    check(bool(meta.get("avcodec_version", "")), "avcodec_version set (FfmpegVideoDecoder linked)")
    check("inits=1" in meta.get("fmv", ""), "fmv= inits=1 (sceMpegInit -> MpegApi::Init)")
    check("init=1" in meta.get("libipu_calls", ""), "libipu_calls init=1 (sceIpuInit -> LibipuApi::Init)")
    check(meta.get("unhandled_mmio") == "0", "unhandled_mmio=0 (IPU window registered)")
    detail = meta.get("park_detail", "")
    check("sceIpuInit" not in detail, "park_detail is not libipu.a sceIpuInit")
    check("sceCdSt" not in detail, "park_detail is not a libcdvd.a sceCdSt* park")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)
    if args.require_subsystem_census:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "verify"))
        from subsystem_reachability import inspect
        counts, errors = inspect(meta)
        for key, value in sorted(counts.items()):
            print("  census %s=%d" % (key, value))
        for error in errors:
            print("  census error: " + error)
        check(not errors, "DMA/IPU/IOP census is complete and reports no unsupported operations")
        check(int(meta.get("fmv_pictures_served", "0")) > 0, "movie HLE served pictures during the observed run")
        def counts(key):
            return dict(part.split("=", 1) for part in meta.get(key, "").split(",") if "=" in part)
        movie, libipu = counts("movie_decoder"), counts("libipu_calls")
        check(meta.get("game_libraries_installed") == "1", "all game library bindings installed")
        check(int(movie.get("created", "0")) > 0, "MovieDecoder owns MPEG context creation")
        check(movie.get("pictures") == meta.get("fmv_pictures_served"), "every served picture uses MovieDecoder")
        check(movie.get("fallback") == "0" and movie.get("invalid_work") == "0",
              "movie path has no fallback or invalid context")
        check(libipu.get("restart") == "0", "movie pictures never enter IPU RestartDMA")

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootBindsFmvRows: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
