#!/usr/bin/env python3
"""RootMixesFmvPcm -- the composition root binds the mixer's FmvPcmInput as the
sceMpeg HLE's FmvPcmSink (INTEGRATION_PLAN row 7.2): a coldboot leg of
--frames N (deterministic profile, headless) must report in run.meta

  * fmv_pcm sink=mixer                 -- the FmvPcmInput, not the tally sink
  * fmv_pcm pushed > 0, mixed > 0      -- the movies' PCM reached the queue and
                                          the SPU2 core consumed it inside MixInto
  * fmv_pcm dropped=0                  -- the queue holds the whole demuxed body
  * fmv_pcm gate_opens >= 1            -- MpegApi opened the gate at a served picture
  * fmv_pcm mixed + queued + (frames discarded by flushes) == pushed, i.e.
                                          mixed <= pushed and queued <= pushed
  * snd_frames == frames * 800         -- the pump cadence is unchanged
  * finalized=1, frames=N              -- the --frames cap ended the leg

Without the binding (the tally sink) the same leg reports sink=tally and no
mixed= field. Exit 77 (SKIP) when the title files are absent.
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
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection (e.g. hle.movie_lifecycle=off: the retained arm this pin measures)")
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_fmv_pcm_")
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
    for key in ("fmv_pcm", "fmv_pictures_served", "snd_frames", "park", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    def num(d, k):
        v = d.get(k, "")
        return int(v) if v.isdigit() else -1

    check(proc.returncode == 0, "bounded run exits 0")
    pcm = parse_fields(meta.get("fmv_pcm", ""))
    check(pcm.get("sink") == "mixer", "fmv_pcm sink=mixer (FmvPcmInput bound as the FmvPcmSink)")
    pushed, mixed, queued, dropped = num(pcm, "pushed"), num(pcm, "mixed"), num(pcm, "queued"), num(pcm, "dropped")
    check(pushed > 0, "fmv_pcm pushed > 0 (the movies' PCM reached the queue)")
    check(mixed > 0, "fmv_pcm mixed > 0 (SpuCore::MixInto consumed FMV frames)")
    check(dropped == 0, "fmv_pcm dropped=0 (the queue held the demuxed body)")
    check(num(pcm, "gate_opens") >= 1, "fmv_pcm gate_opens >= 1 (MpegApi opened the gate)")
    # The title's BVOLL/BVOLR programming (sceSdRemote rSdSetParam 0xF80/0x1080 = 0x7FFF at
    # movie start, 0 at its end) reaches the core-0 shadow on the deterministic leg too; the
    # deterministic lane keeps the boundary-only pump (snd_pumps_fields=0), so the census
    # names the values without asserting audibility here (the play-mode pin does).
    check(pcm.get("bvol_peak", "").lower() == "0x7fff", "fmv_pcm bvol_peak=0x7fff (the title's BVOL value reached the core)")
    check(num(pcm, "bvol_nonzero_sets") >= 2, "fmv_pcm bvol_nonzero_sets >= 2 (BVOLL and BVOLR set)")
    check(meta.get("snd_pumps_fields") == "0", "snd_pumps_fields=0 (no field pump on a deterministic leg)")
    check(0 <= mixed <= pushed and 0 <= queued <= pushed and mixed + queued <= pushed,
          "fmv_pcm mixed + queued <= pushed (the rest went with the flushes)")
    check(meta.get("snd_frames") == str(args.frames * 800), "snd_frames=%d (800 per boundary)" % (args.frames * 800))
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
    print("RootMixesFmvPcm: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
