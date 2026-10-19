#!/usr/bin/env python3
"""RootTriageBundleOnFault -- the crash path writes the TRIAGE BUNDLE on a real
process (ALL-ON; NO EXCEPTIONS). A deterministic headless
coldboot with the harness-only fault injection `--set harness.inject_fault_frame=N`
throws a synthetic fault at frame boundary N; the leg must

  * NOT run to its --frames cap (the runner exits non-zero: the fault aborts the process)
  * finalize run.meta with park=exception, park_detail naming the injected fault,
    frames == N (the boundaries before the fault), finalized=1, triage_bundle=<dir>
  * write <harness-out>/triage/<UTC timestamp>/ with census.txt, drawlist_last200.txt,
    fault.txt, backtrace.txt and run.meta -- every file present and non-empty;
    fault.txt carries the park line, the error text, a faulting draw and the
    producer rows; backtrace.txt is headed by the throwing thread; the bundle's
    run.meta says park=exception
  * print the census at exit: the runner log holds a `census:` block whose rows
    include park=exception (the same rows as run.meta)
  * keep every retained Graphics key's census row (the drawlist ring saw frames)

Also pins the refusal of the knob outside a harness leg (no --harness-out) and
the `--set hle.<key>=off` degradation line (`[config] hle.movie_stop disabled:
requires hle.movie_wait`) on the real binary's stderr. (The key
is hle.movie_wait, not hle.graphics_movie -- the latter cascades to hle.movie_start,
a native-startup consumer, and the service-only play binary REFUSES that by
decision; RootPlayBinaryServiceOnly pins the refusal.)

Exit 77 (SKIP) when the title files are absent.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
FILES = ("census.txt", "drawlist_last200.txt", "fault.txt", "backtrace.txt", "run.meta")


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--fault-frame", type=int, default=20)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
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

    # (a) the knob is a harness-leg opt-in: refused without --harness-out, and
    # (b) a prerequisite turned off degrades its dependents with one logged line.
    probe = subprocess.run([args.binary, "--set", "harness.inject_fault_frame=%d" % args.fault_frame,
                            "--game-files", os.path.abspath(args.game_files)],
                           capture_output=True, text=True, timeout=30)
    print("probe (no harness) exit=%d stderr=%s" % (probe.returncode, probe.stderr.strip().splitlines()[:1]))
    check(probe.returncode == 2 and "harness.inject_fault_frame" in probe.stderr and "--harness-out" in probe.stderr,
          "harness.inject_fault_frame is refused outside a harness leg (exit 2, names the flag)")
    probe = subprocess.run([args.binary, "--help"], capture_output=True, text=True, timeout=30)
    check("--set hle.<key>=off" in probe.stdout and "harness.inject_fault_frame" in probe.stdout,
          "--help documents the per-run `--set hle.<key>=off` bisect form and the fault-injection pin")

    out_dir = tempfile.mkdtemp(prefix="root_triage_bundle_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files),
           "--set", "harness.inject_fault_frame=%d" % args.fault_frame,
           "--set", "hle.movie_wait=off"]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    check(proc.returncode not in (0, 124, 125, 137), "the injected fault ends the process (non-zero exit, not a timeout/kill)")

    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("frames", "finalized", "park", "park_pc", "park_tid", "park_detail", "triage_bundle"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    check(meta.get("park") == "exception", "run.meta park=exception")
    check("injected fault at frame boundary %d" % args.fault_frame in meta.get("park_detail", ""),
          "run.meta park_detail names the injected fault")
    check(meta.get("finalized") == "1", "run.meta finalized=1")
    check(meta.get("frames") == str(args.fault_frame), "run.meta frames=%d (the boundaries before the fault)" % args.fault_frame)
    bundle = meta.get("triage_bundle", "")
    check(bundle.startswith(os.path.join(out_dir, "triage") + os.sep), "run.meta triage_bundle=<harness-out>/triage/<timestamp>")
    triage_root = os.path.join(out_dir, "triage")
    dirs = sorted(os.listdir(triage_root)) if os.path.isdir(triage_root) else []
    check(len(dirs) == 1, "exactly one bundle directory under triage/ (%s)" % ",".join(dirs))
    if dirs:
        bdir = os.path.join(triage_root, dirs[0])
        for name in FILES:
            path = os.path.join(bdir, name)
            ok = os.path.isfile(path) and os.path.getsize(path) > 0
            check(ok, "bundle file present and non-empty: %s%s" % (name, "" if ok else " (MISSING/EMPTY)"))
        fault = read(os.path.join(bdir, "fault.txt")) if os.path.isfile(os.path.join(bdir, "fault.txt")) else ""
        print("fault.txt:\n" + "".join("    " + l + "\n" for l in fault.splitlines()[:8]))
        check(fault.startswith("park=exception park_pc="), "fault.txt: the park line first")
        check("error: injected fault at frame boundary %d" % args.fault_frame in fault, "fault.txt: the error text")
        check("faulting_draw: draw seq=" in fault, "fault.txt: a faulting draw (the ring saw %d frames)" % args.fault_frame)
        check("producer: " in fault and ("graphics_" in fault or "none (" in fault), "fault.txt: the producer rows")
        check("throw_site: noted on thread" in fault, "fault.txt: the throw site was noted on the throwing thread")
        bt = read(os.path.join(bdir, "backtrace.txt")) if os.path.isfile(os.path.join(bdir, "backtrace.txt")) else ""
        check(bt.startswith("throwing thread ") and len(bt.splitlines()) >= 3, "backtrace.txt: throwing thread header + symbol frames")
        bmeta = parse_meta(os.path.join(bdir, "run.meta")) if os.path.isfile(os.path.join(bdir, "run.meta")) else {}
        check(bmeta.get("park") == "exception" and bmeta.get("finalized") == "1", "bundle run.meta: park=exception finalized=1")
        census = read(os.path.join(bdir, "census.txt")) if os.path.isfile(os.path.join(bdir, "census.txt")) else ""
        check("park=exception" in census and "frames=%d" % args.fault_frame in census, "census.txt carries the run.meta rows")
        drawlist = read(os.path.join(bdir, "drawlist_last200.txt")) if os.path.isfile(os.path.join(bdir, "drawlist_last200.txt")) else ""
        check(drawlist.startswith("drawlist ring: frames_closed=") and "frame src_frame=" in drawlist,
              "drawlist_last200.txt: the ring's frame summaries and draws")
    text = read(log) if os.path.isfile(log) else ""
    check("census:\n" in text and "\n  park=exception\n" in text, "the census was printed at exit (`census:` block, park=exception row)")
    check("nativeee: triage bundle written: " in text, "the bundle path was announced")
    check("[config] hle.movie_stop disabled: requires hle.movie_wait" in text,
          "--set hle.movie_wait=off degraded hle.movie_stop with the logged line")

    if failures == 0 and not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootTriageBundleOnFault: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
