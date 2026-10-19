#!/usr/bin/env python3
"""RootPlayBinaryServiceOnly -- the PLAY binary carries no IOP.

`nativeee` is service-only: the IOP kernel / modules, the recompiled STREAM.IRX and
its plugin, the libsd model and the SIF DMA model live in the test-only library
ps2r_iop_models, linked by `nativeee_oracle` and the component oracles alone. This
pin runs the PLAY binary four times (short headless legs; no title state is read):

  1. `--profile deterministic --frames N`: the profile's `hle.startup=retained` pin is the
     oracle binary's -- the play binary runs it as `native` and SAYS so: stderr carries
     `[config] hle.startup=native (the profile's retained pin is the oracle binary's ...)`;
     run.meta: finalized=1, frames=N, iop_linked=0, startup=native, iop_boot=0,
     iop_modules empty, stream_rpc_calls=0/0, libsd_calls empty, iop_clock=frames=0,us=0,
     sif_bound=STREAM_NATIVE@0x12345, stream_transport installed=1 with server_calls==batches.
  2. `--set hle.startup=retained`: REFUSED (exit 2, no run.meta) -- stderr names
     `hle.startup=retained` and `nativeee_oracle`.
  3. `--set hle.music=off` (one of the twelve degrade edges that used to boot the IOP):
     REFUSED (exit 2) -- stderr names `hle.music` and the service-only binary; no leg runs.
  4. `--set iop.dispatch=realtime` (the twelfth edge, reached through hle.audio_mix): REFUSED
     with `iop.dispatch=frame` quoted in the degrade chain.

FAIL on any other outcome; SKIP 77 only without the title files or the binary.
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
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                meta[k] = v
    return meta


def counters(text):
    out = {}
    for part in text.split(","):
        m = re.fullmatch(r"([A-Za-z_0-9]+)=(.*)", part.strip())
        if m is None:
            return {}
        out[m.group(1)] = int(m.group(2)) if re.fullmatch(r"-?\d+", m.group(2)) else m.group(2)
    return out


def judge_leg(returncode, stderr, meta, frames):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "deterministic leg exits 0 (exit=%d)" % returncode)
    check("[config] hle.startup=native (the profile's retained pin is the oracle binary's" in stderr,
          "stderr says the profile's retained pin became native (the service-only binary has no IOP to boot)")
    check(meta.get("finalized") == "1", "finalized=1 (%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("iop_linked") == "0", "iop_linked=0 (the IOP arm is not compiled into the play binary; %s)" % meta.get("iop_linked", "<absent>"))
    check(meta.get("startup") == "native", "startup=native (%s)" % meta.get("startup", "<absent>"))
    check(meta.get("iop_boot") == "0", "iop_boot=0 (%s)" % meta.get("iop_boot", "<absent>"))
    check(meta.get("iop_modules", "<absent>") in ("", "none"), "iop_modules empty (%s)" % meta.get("iop_modules", "<absent>")[:40])
    check(meta.get("stream_rpc_calls") == "0/0", "stream_rpc_calls=0/0 (%s)" % meta.get("stream_rpc_calls", "<absent>"))
    check(meta.get("libsd_calls", "<absent>") in ("", "none"), "libsd_calls empty (%s)" % meta.get("libsd_calls", "<absent>")[:40])
    check(meta.get("iop_clock") == "frames=0,us=0", "iop_clock=frames=0,us=0 (%s)" % meta.get("iop_clock", "<absent>"))
    check(meta.get("iop_hb_beats") == "0", "iop_hb_beats=0 (%s)" % meta.get("iop_hb_beats", "<absent>"))
    check(meta.get("sif_bound", "").startswith("STREAM_NATIVE@0x12345"), "sif_bound=STREAM_NATIVE@0x12345 (%s)" % meta.get("sif_bound", "<absent>"))
    t = counters(meta.get("stream_transport", ""))
    check(t.get("installed") == 1 and t.get("server_calls", -1) == t.get("batches", -2),
          "stream_transport installed=1, server_calls == batches (the owned transport answers 0x12345; %s)" % meta.get("stream_transport", "<absent>")[:100])
    return checks


def judge_refusal(label, returncode, stderr, meta_present, *needles):
    checks = []
    checks.append((returncode == 2, "%s: refused with exit 2 (exit=%d)" % (label, returncode)))
    checks.append((not meta_present, "%s: no leg ran (no run.meta)" % label))
    for n in needles:
        checks.append((n in stderr, "%s: refusal names `%s` (stderr: %s)" % (label, n, stderr.strip().splitlines()[-1][:160] if stderr.strip() else "<empty>")))
    return checks


def selftest():
    good_err = "nativeee: [config] hle.startup=native (the profile's retained pin is the oracle binary's; this service-only binary has no IOP to boot)\n"
    good_meta = {"finalized": "1", "frames": "4", "iop_linked": "0", "startup": "native", "iop_boot": "0", "iop_modules": "",
                 "stream_rpc_calls": "0/0", "libsd_calls": "", "iop_clock": "frames=0,us=0", "iop_hb_beats": "0",
                 "sif_bound": "STREAM_NATIVE@0x12345", "stream_transport": "installed=1,batches=3,server_calls=3"}
    ok = all(c for c, _ in judge_leg(0, good_err, good_meta, 4))
    bad = dict(good_meta); bad["iop_linked"] = "1"; bad["iop_boot"] = "1"
    ok &= sum(1 for c, _ in judge_leg(0, good_err, bad, 4) if not c) == 2
    ok &= all(c for c, _ in judge_refusal("x", 2, "nativeee: hle.startup=retained: ... nativeee_oracle\n", False, "hle.startup=retained", "nativeee_oracle"))
    ok &= not all(c for c, _ in judge_refusal("x", 0, "", True, "hle.music"))
    print("root_play_binary_service_only selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the PLAY nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("SKIP: binary missing: %s" % args.binary)
        return 77

    root = tempfile.mkdtemp(prefix="root_play_binary_service_only_")
    game = ["--game-files", os.path.abspath(args.game_files), "--headless"]
    checks = []

    def run(label, out, extra):
        cmd = [args.binary] + game + ["--harness-out", out, "--frames", str(args.frames)] + extra
        print("run [%s]: %s" % (label, " ".join(cmd)))
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print("  TIMEOUT after %ds" % args.timeout)
            return 124, "", False, {}
        meta_path = os.path.join(out, "run.meta")
        present = os.path.isfile(meta_path)
        meta = parse_meta(meta_path) if present else {}
        for line in (p.stderr or "").strip().splitlines()[-3:]:
            print("  stderr: " + line[:200])
        return p.returncode, p.stderr or "", present, meta

    rc, err, present, meta = run("deterministic", os.path.join(root, "det"), ["--profile", "deterministic"])
    for key in ("frames", "finalized", "iop_linked", "startup", "iop_boot", "iop_modules", "stream_rpc_calls", "libsd_calls",
                "iop_clock", "iop_hb_beats", "sif_bound", "stream_transport"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")[:120]))
    checks += judge_leg(rc, err, meta, args.frames)
    rc, err, present, _ = run("retained", os.path.join(root, "retained"), ["--set", "hle.startup=retained"])
    checks += judge_refusal("--set hle.startup=retained", rc, err, present, "hle.startup=retained", "nativeee_oracle")
    rc, err, present, _ = run("music_off", os.path.join(root, "music_off"), ["--set", "hle.music=off"])
    checks += judge_refusal("--set hle.music=off", rc, err, present, "hle.startup=native requires hle.music=on", "service-only")
    rc, err, present, _ = run("dispatch", os.path.join(root, "dispatch"), ["--set", "iop.dispatch=realtime"])
    checks += judge_refusal("--set iop.dispatch=realtime", rc, err, present, "requires iop.dispatch=frame", "service-only")

    failures = 0
    for ok, what in checks:
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootPlayBinaryServiceOnly: FAIL (%d) -- out %s" % (failures, root))
        return 1
    print("RootPlayBinaryServiceOnly: ALL PASS")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
