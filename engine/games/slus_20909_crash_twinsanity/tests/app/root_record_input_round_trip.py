#!/usr/bin/env python3
"""RootRecordInputRoundTrip: the play-session recorder command, end to end on the actual binary.

A deterministic headless cold-boot leg replays a small pad script (`--inputs`) and records the
served pad into an EXPLICIT file (`--record-input <file>`, the play-session command's form). The pins:
  * run.meta `pad_record_path` names exactly the explicit file; `pad_record_lines` > 0;
    `pad_record_start` <= the script's first tick and `pad_record_last` >= its last tick
    (the leg served every scripted tick);
  * the recording starts with the `#` header line and, replayed with the padlog grammar, never
    holds a button or axis value the script does not hold at that tick (no phantom press: the
    recorder writes the served sample and the served sample is the script's) and records every
    scripted range at least once inside it (the pad is stamped once per boundary at the domain
    tick, and demand ticks between boundaries are never stamped, so a recorded range's ends are
    the first/last STAMPED ticks inside the scripted one -- a per-tick equality is not the contract);
  * a second leg with the same `--record-input <file>` exits 2 with the refusal
    "cannot create input recording ... (an existing file is never overwritten)" and the first
    recording's bytes are unchanged.
Selftest pins the judge (pure functions). SKIP 77 without the title files.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
SCRIPT = ["# RootRecordInputRoundTrip script (ticks are logical input ticks)",
          "5-15:CROSS", "10-25:START", "8-20:LX=200", "12-18:RY=40"]
SCRIPT_FIRST, SCRIPT_LAST = 5, 25
REFUSAL = "an existing file is never overwritten"


def parse_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def held_states(lines, first, last):
    """Per-tick (frozenset(buttons), axes dict) over [first, last] from padlog range lines
    (the replayer's rule: a button is held over F1..F2 inclusive; the LAST matching axis
    segment wins; unknown tokens are skipped)."""
    out = {t: (set(), {}) for t in range(first, last + 1)}
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        for tok in line.split(","):
            tok = tok.strip()
            if ":" not in tok or "-" not in tok.split(":", 1)[0]:
                continue
            rng, what = tok.split(":", 1)
            f1, f2 = rng.split("-", 1)
            try:
                f1, f2 = int(f1), int(f2)
            except ValueError:
                continue
            for t in range(max(f1, first), min(f2, last) + 1):
                if "=" in what:
                    ax, val = what.split("=", 1)
                    out[t][1][ax] = int(val)
                else:
                    out[t][0].add(what)
    return {t: (frozenset(b), tuple(sorted(a.items()))) for t, (b, a) in out.items()}


def script_ranges():
    out = []
    for line in SCRIPT:
        if line.startswith("#"):
            continue
        rng, token = line.split(":", 1)
        f1, f2 = rng.split("-", 1)
        out.append(((int(f1), int(f2)), token))
    return out


def judge(meta, explicit_path, recording_lines, second_exit, second_log, digest_before, digest_after):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta.get("pad_record_path") == explicit_path,
          "run.meta pad_record_path=%s is the explicit file %s" % (meta.get("pad_record_path", "<absent>"), explicit_path))
    lines = int(meta.get("pad_record_lines", "0") or 0)
    check(lines > 0, "pad_record_lines=%d > 0" % lines)
    start = int(meta.get("pad_record_start", "-1") or -1)
    last = int(meta.get("pad_record_last", "-1") or -1)
    check(0 <= start <= SCRIPT_FIRST, "pad_record_start=%d <= first scripted tick %d" % (start, SCRIPT_FIRST))
    check(last >= SCRIPT_LAST, "pad_record_last=%d >= last scripted tick %d" % (last, SCRIPT_LAST))
    check(bool(recording_lines) and recording_lines[0].startswith("#"), "recording begins with the # header line")
    if start >= 0 and last >= start:
        want = held_states(SCRIPT, start, last)
        got = held_states(recording_lines, start, last)
        phantom = [t for t in range(start, last + 1)
                   if not got[t][0] <= want[t][0] or any(dict(want[t][1]).get(ax) != v for ax, v in got[t][1])]
        check(not phantom, "recording holds nothing the script does not hold, ticks %d..%d%s"
              % (start, last, "" if not phantom else " (phantom at %s)" % phantom[:8]))
        missing = []
        for rng, token in script_ranges():
            f1, f2 = rng
            if "=" in token:
                ax, val = token.split("=", 1)
                seen = any(dict(got[t][1]).get(ax) == int(val) for t in range(f1, f2 + 1) if t in got)
            else:
                seen = any(token in got[t][0] for t in range(f1, f2 + 1) if t in got)
            if not seen:
                missing.append("%d-%d:%s" % (f1, f2, token))
        check(not missing, "recording carries every scripted range%s" % ("" if not missing else " (missing %s)" % missing))
    else:
        check(False, "recording window unknown (pad_record_start/last absent)")
    check(second_exit == 2, "second leg with the same --record-input exits 2 (got %s)" % second_exit)
    check(REFUSAL in second_log, "second leg refuses: '%s' in its log" % REFUSAL)
    check(digest_before == digest_after, "the first recording's bytes are unchanged by the refused leg")
    return results


def selftest():
    ok = True
    rec = ["# header", "5-15:CROSS", "10-25:START", "8-20:LX=200", "12-18:RY=40"]
    meta = {"pad_record_path": "/x/s.padlog", "pad_record_lines": "4", "pad_record_start": "0", "pad_record_last": "60"}
    r = judge(meta, "/x/s.padlog", rec, 2, "nativeee: cannot create input recording /x/s.padlog (an existing file is never overwritten)", "d", "d")
    ok &= all(x for x, _ in r)
    # the recorder's 60-tick flush splits a held range: identical per-tick states
    split = ["# header", "5-9:CROSS", "10-15:CROSS", "10-25:START", "8-20:LX=200", "12-18:RY=40"]
    r = judge(meta, "/x/s.padlog", split, 2, REFUSAL, "d", "d")
    ok &= all(x for x, _ in r)
    # a phantom press, a missing axis, a different path, an overwritten file: each fails exactly its pin
    # stamped ticks only inside the scripted ranges (demand ticks between boundaries): accepted
    sparse = ["# header", "6-14:CROSS", "11-24:START", "9-19:LX=200", "13-17:RY=40"]
    r = judge(meta, "/x/s.padlog", sparse, 2, REFUSAL, "d", "d")
    ok &= all(x for x, _ in r)
    bad = ["# header", "5-16:CROSS", "10-25:START", "8-20:LX=200", "12-18:RY=40"]
    fails = [w for x, w in judge(meta, "/x/s.padlog", bad, 2, REFUSAL, "d", "d") if not x]
    ok &= len(fails) == 1 and "phantom" in fails[0]
    bad = ["# header", "5-15:CROSS", "10-25:START", "8-20:LX=201", "12-18:RY=40"]
    fails = [w for x, w in judge(meta, "/x/s.padlog", bad, 2, REFUSAL, "d", "d") if not x]
    ok &= len(fails) == 2 and any("phantom" in w for w in fails) and any("8-20:LX=200" in w for w in fails)
    bad = ["# header", "5-15:CROSS", "10-25:START", "12-18:RY=40"]
    fails = [w for x, w in judge(meta, "/x/s.padlog", bad, 2, REFUSAL, "d", "d") if not x]
    ok &= len(fails) == 1 and "8-20:LX=200" in fails[0]
    fails = [w for x, w in judge(dict(meta, pad_record_path="/x/input.padlog"), "/x/s.padlog", rec, 2, REFUSAL, "d", "d") if not x]
    ok &= len(fails) == 1 and "explicit file" in fails[0]
    fails = [w for x, w in judge(meta, "/x/s.padlog", rec, 0, "", "d", "e") if not x]
    ok &= len(fails) == 3
    # a leg too short for the script
    fails = [w for x, w in judge(dict(meta, pad_record_last="20"), "/x/s.padlog", rec, 2, REFUSAL, "d", "d") if not x]
    ok &= any("pad_record_last" in w for w in fails)
    # no header / no lines
    fails = [w for x, w in judge(dict(meta, pad_record_lines="0"), "/x/s.padlog", [], 2, REFUSAL, "d", "d") if not x]
    ok &= any("header" in w for w in fails) and any("pad_record_lines" in w for w in fails)
    print("root_record_input_round_trip selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def run_leg(args, out_dir, log, script_path, explicit_path):
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
           "--inputs", script_path, "--record-input", explicit_path,
           "--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(min(args.timeout, 90))
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=61)
    ap.add_argument("--timeout", type=int, default=240)
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

    root = tempfile.mkdtemp(prefix="root_record_input_round_trip_")
    script_path = os.path.join(root, "script.padlog")
    with open(script_path, "w") as f:
        f.write("\n".join(SCRIPT) + "\n")
    explicit_path = os.path.join(root, "session", "session-1.padlog")   # parent dir created by the host
    out1, out2 = os.path.join(root, "leg1"), os.path.join(root, "leg2")
    log1, log2 = os.path.join(root, "leg1.log"), os.path.join(root, "leg2.log")
    exit1 = run_leg(args, out1, log1, script_path, explicit_path)
    meta_path = os.path.join(out1, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "pad_record_path", "pad_record_start", "pad_record_last", "pad_record_lines",
                "pad_input", "park", "park_detail"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    recording = []
    digest_before = digest_after = None
    if os.path.isfile(explicit_path):
        with open(explicit_path) as f:
            recording = [l.rstrip("\n") for l in f]
        digest_before = sha256(explicit_path)
        print("recording (%d lines):" % len(recording))
        for l in recording[:12]:
            print("  " + l)
    else:
        print("recording file absent: %s" % explicit_path)
    exit2 = run_leg(args, out2, log2, script_path, explicit_path)
    second_log = open(log2).read() if os.path.isfile(log2) else ""
    if os.path.isfile(explicit_path):
        digest_after = sha256(explicit_path)
    failures = 0
    print("first leg exit=%d finalized=%s" % (exit1, meta.get("finalized", "<absent>")))
    for ok, what in judge(meta, explicit_path, recording, exit2, second_log, digest_before, digest_after):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if exit1 != 0 or meta.get("finalized") != "1":
        print("  FAIL first leg exit=%d finalized=%s (expected 0 / 1)" % (exit1, meta.get("finalized")))
        failures += 1
    if failures:
        print("RootRecordInputRoundTrip: FAIL (%d) -- kept at %s" % (failures, root))
        return 1
    shutil.rmtree(root, ignore_errors=True)
    print("RootRecordInputRoundTrip: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
