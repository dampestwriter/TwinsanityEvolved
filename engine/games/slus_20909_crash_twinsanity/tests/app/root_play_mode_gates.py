#!/usr/bin/env python3
"""RootGatesPlayModeDevices -- the composition root opens NO host device on a
harness leg and folds the decoder version into the leg identity
(INTEGRATION_PLAN rows 7.1, 7.5, 7.8): a coldboot --frames N leg
(deterministic profile, headless) must report in run.meta

  * audio_sink=none                     -- SdlAudioSink::Open is play-mode only
  * audio_ring pushed == N * 800        -- the mixer output of every boundary
                                           was ALSO pushed into the AudioPullRing
                                           (the side effect that feeds the device)
  * pad_input in {neutral, replay}      -- the live InputSystem source is never
                                           constructed on a headless leg
  * avcodec_version=<x.y.z>             -- stamped at Arm (present without FMV)
  * env_hash == FNV-1a64 fold of the env_ps2 entries followed by
    `avcodec_version=<x.y.z>` (each entry then ";") -- the decoder dependency is
    part of the same-engine meta gate while env_ps2 stays the Config fingerprint
  * finalized=1, frames=N

Exit 77 (SKIP) when the title files are absent.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3


def fnv1a64(data, h):
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def fold_entries(entries):
    h = FNV_OFFSET
    for e in entries:
        h = fnv1a64(e.encode("utf-8"), h)
        h = fnv1a64(b";", h)
    return h


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=50)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_play_gates_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames),
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
    for key in ("audio_sink", "audio_ring", "pad_input", "avcodec_version", "env_ps2", "env_hash", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("audio_sink") == "none", "audio_sink=none (no device on a headless leg)")
    ring = parse_fields(meta.get("audio_ring", ""))
    check(ring.get("pushed") == str(args.frames * 800), "audio_ring pushed=%d (800 per boundary into the ring)" % (args.frames * 800))
    check(ring.get("pulled") == "0", "audio_ring pulled=0 (no device callback ran)")
    check(meta.get("pad_input") in ("neutral", "replay"), "pad_input is neutral|replay (no live source headless)")
    version = meta.get("avcodec_version", "")
    check(re.fullmatch(r"\d+\.\d+\.\d+", version) is not None, "avcodec_version=<major.minor.micro> stamped")
    check("avcodec_version" not in meta.get("env_ps2", ""), "env_ps2 stays the Config fingerprint (no avcodec entry)")
    entries = [e for e in meta.get("env_ps2", "").split(";") if e]
    expected = fold_entries(entries + ["avcodec_version=%s" % version])
    check(meta.get("env_hash", "") == "0x%016x" % expected,
          "env_hash folds env_ps2 entries + avcodec_version (expected 0x%016x)" % expected)
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootGatesPlayModeDevices: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
