#!/usr/bin/env python3
"""RootFootprintBounded -- the host-footprint pin: a deterministic
headless coldboot leg must keep its physical footprint bounded by the work of a
frame, never by the number of frames.

The leg's run.meta carries the `footprint=` census written by the EE bootstrap
(`b60_mb` the 60th boundary's footprint, `last_mb`, `peak_mb`, `final_mb`, and
`gsops_retained_bytes` = the register-op recorder's retained storage) and the
`footprint_series=` samples (one per 60 boundaries). The pin:

  * gsops_retained_bytes == 0     -- the recorder runs hash-only in a leg (the
                                     defect: 24 bytes retained per sink call, ~8-11
                                     MB per title frame, OS kill at ~4096 frames);
  * peak_mb <= --peak-mb           -- the whole leg's peak stays under the budget
                                     (before the fix a 400-frame leg already held
                                     ~1.2 GB of records on top of the ~0.4 GB base);
  * the series' last sample grows no more than --tail-growth-mb over its
    predecessor (the boot movies allocate their decode buffers once; an unbounded
    retainer shows a constant slope to the end).

Exit 77 (SKIP) only when the title files are absent. `--print` dumps the census.
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
        for ln in f:
            ln = ln.rstrip("\n")
            if "=" in ln:
                k, v = ln.split("=", 1)
                meta[k] = v
    return meta


def parse_kv_list(text):
    """'a=1,b=2' -> {'a': 1, 'b': 2} (pure; ints)."""
    out = {}
    for part in text.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
    return out


def parse_series(text):
    """'b0:78,b60:79' -> [(0, 78), (60, 79)] (pure)."""
    out = []
    if not text or text == "none":
        return out
    for part in text.split(","):
        b, mb = part.split(":", 1)
        out.append((int(b[1:]), int(mb)))
    return out


def judge(meta, peak_mb, tail_growth_mb, frames):
    """(ok, lines) from a parsed run.meta (pure; pinned by --selftest)."""
    lines = []
    ok = True
    if meta.get("finalized") != "1" or int(meta.get("frames", "0")) != frames:
        lines.append("FAIL: leg did not finalize at %d frames (frames=%s finalized=%s)"
                     % (frames, meta.get("frames"), meta.get("finalized")))
        return False, lines
    fp = parse_kv_list(meta.get("footprint", ""))
    series = parse_series(meta.get("footprint_series", ""))
    lines.append("footprint=%s" % meta.get("footprint", "<missing>"))
    lines.append("footprint_series=%s" % meta.get("footprint_series", "<missing>"))
    if "gsops_retained_bytes" not in fp or "peak_mb" not in fp:
        lines.append("FAIL: footprint census missing")
        return False, lines
    if fp["gsops_retained_bytes"] != 0:
        ok = False
        lines.append("FAIL: the register-op recorder retained %d bytes in a leg (hash-only expected)"
                     % fp["gsops_retained_bytes"])
    if fp["peak_mb"] > peak_mb:
        ok = False
        lines.append("FAIL: peak footprint %d MiB > budget %d MiB" % (fp["peak_mb"], peak_mb))
    if len(series) >= 2:
        growth = series[-1][1] - series[-2][1]
        lines.append("tail growth b%d->b%d: %+d MiB (limit %d)" % (series[-2][0], series[-1][0], growth, tail_growth_mb))
        if growth > tail_growth_mb:
            ok = False
            lines.append("FAIL: footprint still growing at the leg's tail")
    else:
        ok = False
        lines.append("FAIL: footprint_series has fewer than two samples")
    return ok, lines


def selftest():
    good = {"frames": "400", "finalized": "1",
            "footprint": "b60_mb=79,last_mb=383,peak_mb=385,final_mb=383,gsops_retained_bytes=0",
            "footprint_series": "b0:78,b60:79,b120:206,b180:325,b240:385,b300:383,b360:383"}
    ok, _ = judge(good, 1024, 120, 400)
    r = ok
    bad_retained = dict(good, footprint="b60_mb=79,last_mb=1300,peak_mb=1300,final_mb=1300,gsops_retained_bytes=937000000")
    ok, lines = judge(bad_retained, 1024, 120, 400)
    r &= (not ok) and any("retained" in l for l in lines) and any("peak" in l for l in lines)
    bad_slope = dict(good, footprint_series="b0:78,b60:79,b120:206,b180:325,b240:385,b300:510,b360:640")
    ok, lines = judge(bad_slope, 1024, 120, 400)
    r &= (not ok) and any("tail" in l and "FAIL" in l for l in lines)
    ok, _ = judge(dict(good, finalized="0"), 1024, 120, 400)
    r &= not ok
    r &= parse_series("none") == [] and parse_series("b0:1,b60:2") == [(0, 1), (60, 2)]
    print("root_footprint_bounded selftest: %s" % ("PASS" if r else "FAIL"))
    return 0 if r else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--peak-mb", type=int, default=1024)
    ap.add_argument("--tail-growth-mb", type=int, default=120)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection forwarded to the binary (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not (args.binary and args.game_files and args.runner):
        ap.error("--binary, --game-files and --runner are required")
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    out_dir = tempfile.mkdtemp(prefix="root_footprint_bounded_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    meta_path = os.path.join(out_dir, "run.meta")
    rc = 1
    if proc.returncode != 0:
        print("FAIL: runner exit %d (see %s)" % (proc.returncode, log))
    elif not os.path.isfile(meta_path):
        print("FAIL: run.meta missing")
    else:
        ok, lines = judge(parse_meta(meta_path), args.peak_mb, args.tail_growth_mb, args.frames)
        for ln in lines:
            print(ln)
        rc = 0 if ok else 1
    if rc == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
        print("RootFootprintBounded: PASS")
    else:
        print("RootFootprintBounded: FAIL (leg kept at %s)" % out_dir)
    return rc


if __name__ == "__main__":
    sys.exit(main())
