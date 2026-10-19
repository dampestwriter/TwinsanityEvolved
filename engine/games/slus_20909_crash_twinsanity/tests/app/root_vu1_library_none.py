#!/usr/bin/env python3
"""RootVu1LibraryNone -- the `--vu1-library none` harness knob (a
CLI knob beside --headless/--frames, NOT a Config key) reaches the composition
root: the VU1 HLE library consult is off, the registry is effectively empty and
every VU1 program runs on the engine.vu1_mode fallback tier.

Runs the built `nativeee` binary on a deterministic headless anchor leg (the
artdet anchor, OLD save_states read-only; SKIP 77 when absent) with
`--vu1-library none` and asserts on run.meta:

  * vu1_library=none        -- the knob reached the root (default legs say title)
  * vu1_hle=0/0/0           -- no HLE run, no miss: the hook was never set
  * vu1_tier=cached         -- the deterministic profile's fallback tier
  * finalized=1, frames=N   -- the leg ran to its cap on the fallback tier

Then a control: the same leg WITHOUT the knob reports vu1_library=title and a
non-zero vu1_hle runs count (the knob's absence keeps the lanes unchanged).
Never SKIPs for a leg that started: a crash or stall is a failure.
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
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def run_leg(args, tag, extra):
    out_dir = tempfile.mkdtemp(prefix="root_vu1_library_%s_" % tag)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--load-state", os.path.abspath(args.anchor),
           "--game-files", os.path.abspath(args.game_files)] + extra
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run[%s]: %s" % (tag, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "40"
    proc = subprocess.run(cmd, env=env)
    print("runner[%s] exit=%d" % (tag, proc.returncode))
    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "vu1_library", "vu1_tier", "vu1_hle", "wall_ms"):
        print("run.meta[%s] %s=%s" % (tag, key, meta.get(key, "<absent>")))
    return proc.returncode, meta, out_dir, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="artdet_f37152.ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection forwarded to the binary (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
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

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    rc, meta, out_none, log_none = run_leg(args, "none", ["--vu1-library", "none"])
    check(rc == 0, "[none] bounded run exits 0")
    check(meta.get("finalized") == "1", "[none] finalized=1 (the leg ran to its cap on the fallback tier)")
    check(meta.get("frames") == str(args.frames), "[none] frames=%d" % args.frames)
    check(meta.get("vu1_library") == "none", "[none] vu1_library=none (the knob reached the root)")
    check(meta.get("vu1_tier") == "cached", "[none] vu1_tier=cached (the deterministic profile's fallback tier)")
    check(re.fullmatch(r"0/0/0(/.*)?", meta.get("vu1_hle", "")) is not None,
          "[none] vu1_hle=0/0/... (no HLE run and no miss: the hook was never set)")

    rc2, meta2, out_title, log_title = run_leg(args, "title", [])
    check(rc2 == 0, "[title] bounded run exits 0")
    check(meta2.get("finalized") == "1", "[title] finalized=1")
    check(meta2.get("vu1_library") == "title", "[title] vu1_library=title (the default consults the title library)")
    m = re.match(r"(\d+)/(\d+)/(\d+)", meta2.get("vu1_hle", ""))
    check(m is not None and int(m.group(1)) > 0, "[title] vu1_hle runs > 0 (the control consulted the library)")

    if failures == 0:
        for d, lg in ((out_none, log_none), (out_title, log_title)):
            shutil.rmtree(d, ignore_errors=True)
            try:
                os.remove(lg)
            except OSError:
                pass
    else:
        print("artifacts kept: %s %s %s %s" % (out_none, log_none, out_title, log_title))
    print("RootVu1LibraryNone: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
