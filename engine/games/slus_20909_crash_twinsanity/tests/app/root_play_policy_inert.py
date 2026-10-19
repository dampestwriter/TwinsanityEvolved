#!/usr/bin/env python3
"""RootPlayPolicyInert -- the deterministic byte-identity pin that guards the
Play-policy-only changes ((a)+(b) and the row 6.7 present policy): a
deterministic headless coldboot leg `--frames 130` must reproduce the recorded
fixture `tests/app/fixtures/coldboot_130_lanes.txt` LINE FOR LINE on the
`mem.mhash`, `audio.ahash` and `frames.fhash` lanes. The fixture was written
from the binary BEFORE the Play-policy changes; a difference on any line means a
deterministic code path moved.

The fixture file holds the three lanes as sections:

  [mem.mhash]      0,0x...  60,0x...  120,0x...
  [audio.ahash]    one line per 4800-frame window
  [frames.fhash]   one line per boundary (130)

Exit 77 (SKIP) only when the title files are absent.
With --advisory-mem, the original memory fixture is still
compared and differences printed; frame/audio fixtures remain blocking.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
LANES = ("mem.mhash", "audio.ahash", "frames.fhash")


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\n") for ln in f if ln.strip()]


def read_fixture(path):
    lanes = {}
    cur = None
    for ln in read_lines(path):
        if ln.startswith("[") and ln.endswith("]"):
            cur = ln[1:-1]
            lanes[cur] = []
        elif cur is not None:
            lanes[cur].append(ln)
    return lanes


def write_fixture(path, leg_dir):
    with open(path, "w", encoding="utf-8") as f:
        for lane in LANES:
            f.write("[%s]\n" % lane)
            for ln in read_lines(os.path.join(leg_dir, lane)):
                f.write(ln + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--frames", type=int, default=130)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable; e.g. the "
                         "retained Input/Save/libmc arm whose boot timing this fixed-frame pin was recorded under)")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--record", action="store_true", help="(re)write the fixture from this binary")
    ap.add_argument("--advisory-mem", action="store_true",
                    help="Legacy policy: report memory differences while enforcing frame/audio identity")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_play_policy_inert_")
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
    env["RB_STALL_S"] = "60"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    for lane in LANES:
        check(os.path.isfile(os.path.join(out_dir, lane)), "%s written" % lane)
    if failures:
        print("artifacts kept: %s %s" % (out_dir, log))
        print("RootPlayPolicyInert: FAIL")
        return 1

    if args.record:
        write_fixture(args.fixture, out_dir)
        print("fixture recorded: %s" % args.fixture)

    if not os.path.isfile(args.fixture):
        print("FAIL: fixture missing: %s" % args.fixture)
        print("artifacts kept: %s %s" % (out_dir, log))
        return 1
    fixture = read_fixture(args.fixture)
    for lane in LANES:
        want = fixture.get(lane, [])
        got = read_lines(os.path.join(out_dir, lane))
        same = want == got
        check(len(want) > 0, "%s fixture has %d lines" % (lane, len(want)))
        if lane == "mem.mhash" and args.advisory_mem:
            print("  ADVISORY %s %s to the fixture (%d lines)" %
                  (lane, "IDENTICAL" if same else "DIFFERENT", len(got)))
            check(len(got) == len(want), "all expected memory checkpoints recorded")
        else:
            check(same, "%s IDENTICAL to the fixture (%d lines)" % (lane, len(got)))
        if not same:
            n = min(len(want), len(got))
            first = next((i for i in range(n) if want[i] != got[i]), n)
            if first < n:
                print("    first difference at index %d: fixture %r vs leg %r" % (first, want[first], got[first]))
            else:
                print("    line counts differ: fixture %d vs leg %d" % (len(want), len(got)))
    mem = read_lines(os.path.join(out_dir, "mem.mhash"))
    print("  mem.mhash: " + " / ".join(mem))

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootPlayPolicyInert: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
