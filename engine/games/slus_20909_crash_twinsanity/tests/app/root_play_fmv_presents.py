#!/usr/bin/env python3
"""root_play_fmv_presents.py -- play mode shows EVERY movie picture.

Hardware scans DISPFB out on every vblank whether or not the EE's frame loop
runs; a movie never crosses that loop (547 pictures inside ~10 boundaries), so
a boundary-only present shows one picture per boundary (the 2026-10-02 report:
"the FMVs are not running, at most a single frame of the second one"). The
composition root's play-mode DISPLAY TICK presents the scanout on every field the
FMV pace park waits through and on every late serve (`display_ticks=<n>,<late>`),
and the pacer re-anchors after 4 periods of host debt (`fmv_pacer_reanchors=`) so
a slow host plays the movie in slow motion instead of draining it as a blur.

A play-mode coldboot `--frames N` through both movies (GL window) must report:
  * fmv=...served_total=547... (both movies served to the title)
  * display_ticks >= served_total - 20 (every picture but the priming serves and
    the last few got a window present)
  * frames=N, finalized=1 (the display ticks never touch the frames lane / cap)
The movie's SOUND (the same leg): the title programs the SPU2 core-0 external
input volume BVOLL/BVOLR to 0x7FFF at movie start through sceSdRemote
(rSdSetParam 0xF80 / 0x1080) and back to 0 at its end; the SPU2 mixes 48000
frames a second whether or not the EE passes a frame boundary, so the root pumps
the mixer once per display field in play mode (the boundary pump alone would mix
the queued PCM only after BVOL went back to 0: silence). Must report:
  * fmv_pcm=... bvol_peak=0x7fff, bvol_nonzero_sets >= 2 (the console's values)
  * fmv_pcm=... mixed_audible > 0 (movie PCM mixed while BVOL was non-zero)
  * snd_pumps_fields > 1000 (one mixer quantum per field the EE parked through)
  * audio_device=quanta=<n>,nonsilent=<m> with m > 0 (sound reached the device ring)

Verdict rules (cutover 2026-10-08: a crashed game process is never a skip):
  * FAIL whenever the runner exits non-zero (a crash, SIGABRT 134, a kill by
    run_bounded.sh 124/125/137) or run.meta is missing or not finalized=1.
  * SKIP 77 only for the documented missing-asset case (the title files are
    absent) or for a display-less host: the run COMPLETED (exit 0, finalized=1)
    and the root reported video=stub (no window could be created).
`--selftest` runs this judge against a fake runner (tests/app/smoke_fake_runner.py).
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


def completed_without_window(returncode, meta):
    """The only non-asset SKIP: the game process ran to its cap and finalized, but the root
    reported video=stub (a display-less host). A dead or unfinalized process never qualifies."""
    return returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub"


def kv(text):
    return dict(item.split("=", 1) for item in text.split(",") if "=" in item)


def judge(returncode, meta, frames, settings):
    """Returns ([(ok, what)], info_lines). The exit code and finalized=1 pins come first so a
    crash reads as its own failure; every other pin still prints for the record."""
    checks, info = [], []

    def check(ok, what):
        checks.append((bool(ok), what))

    n = frames
    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("video") == "gl", "video=gl (a window existed; video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("frames") == str(n), "frames=%d (the display ticks never count as frames)" % n)
    served = -1
    m = re.search(r"served_total=(\d+)", meta.get("fmv", ""))
    if m:
        served = int(m.group(1))
    check(served >= 500, "fmv served_total=%d >= 500 (both movies served inside %d boundaries)" % (served, n))
    m = re.fullmatch(r"(\d+),(\d+)", meta.get("display_ticks", ""))
    if m:
        ticks, late = int(m.group(1)), int(m.group(2))
        check(ticks >= served - 20, "display_ticks=%d >= served_total - 20 (every picture presented)" % ticks)
        info.append("info: %d of the display ticks were late serves; fmv_pacer_reanchors=%s" % (late, meta.get("fmv_pacer_reanchors")))
    else:
        check(False, "display_ticks=<n>,<late> published")
    check(re.fullmatch(r"\d+", meta.get("fmv_pacer_reanchors", "")) is not None, "fmv_pacer_reanchors=<n> published")

    # Either the retained register owner or the selected software owner must
    # receive the source gain and consume audible PCM in the common mixer.
    pcm = meta.get("fmv_pcm", "")
    info.append("run.meta fmv_pcm=%s" % pcm)
    fields = kv(pcm)
    owned = kv(meta.get("movie_audio", ""))
    if meta.get("movie_lifecycle_binding") == "on":
        check(owned.get("selected") == "1" and owned.get("peak") == "32767" and
              int(owned.get("nonzero_changes", "0")) >= 2, "owned Movie gain handles both starts at the source level")
        check(owned.get("model_pops") == "0" and int(owned.get("audible_frames", "0")) > 0,
              "selected Movie consumes audible PCM without retained input execution")
    else:
        check(fields.get("bvol_peak", "").lower() == "0x7fff", "retained BVOL reaches the source's full gain")
        check(int(fields.get("bvol_nonzero_sets", "0")) >= 2, "retained BVOLL and BVOLR are programmed")
    check(int(fields.get("mixed_audible", "0")) > 0, "fmv_pcm mixed_audible > 0 (movie PCM mixed while BVOL was non-zero)")
    pumps_fields = int(meta.get("snd_pumps_fields", "0"))
    info.append("run.meta snd_pumps=%s snd_pumps_fields=%d audio_device=%s audio_ring=%s" %
                (meta.get("snd_pumps"), pumps_fields, meta.get("audio_device"), meta.get("audio_ring")))
    check(pumps_fields > 1000, "snd_pumps_fields > 1000 (the mixer pumped per display field)")
    m = re.fullmatch(r"quanta=(\d+),nonsilent=(\d+)", meta.get("audio_device", ""))
    check(m is not None and int(m.group(2)) > 0, "audio_device nonsilent quanta > 0 (sound reached the device ring)")

    if "hle.movie_lifecycle=on" in settings:
        check(meta.get("movie_lifecycle_binding") == "on", "owned movie lifecycle selected")
        commands = kv(meta.get("ipu_commands", ""))
        expected = {"bclr", "idec", "bdec", "vdec", "fdec", "setiq", "setvq", "csc", "pack", "setth", "unknown"}
        check(set(commands) == expected and all(value == "0" for value in commands.values()),
              "every IPU command dispatch counter is present and zero")
        movie = kv(meta.get("movie_decoder", ""))
        check(int(movie.get("started", "0")) == 2 and int(movie.get("closed", "0")) == 2,
              "owned start and close actually handled both movies")
        check(all(movie.get(key) == "0" for key in ("fallback", "continuations", "start_failed", "decode_errors")),
              "no generated lifecycle fallback or decoder error")
        check(meta.get("fmv_abort_hold_ticks") == "0", "the replaced abort wrapper never executed")
    return checks, info


# A completed retained-arm run (the shape RootPlayFmvPresents reads on a green tree).
GOOD_META = ("frames=200\nfinalized=1\nvideo=gl\nfmv=served_total=547\ndisplay_ticks=540,12\n"
             "fmv_pacer_reanchors=0\nfmv_pcm=bvol_peak=0x7fff,bvol_nonzero_sets=4,mixed_audible=9000\n"
             "snd_pumps=1200\nsnd_pumps_fields=1100\naudio_device=quanta=900,nonsilent=800\n")
# The same run with the owned lifecycle selected (RootMovieLifecyclePlay's --set).
GOOD_OWNED_META = GOOD_META + (
    "movie_lifecycle_binding=on\nmovie_audio=selected=1,peak=32767,nonzero_changes=2,model_pops=0,audible_frames=5000\n"
    "ipu_commands=bclr=0,idec=0,bdec=0,vdec=0,fdec=0,setiq=0,setvq=0,csc=0,pack=0,setth=0,unknown=0\n"
    "movie_decoder=started=2,closed=2,fallback=0,continuations=0,start_failed=0,decode_errors=0\n"
    "fmv_abort_hold_ticks=0\n")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    frames = ["--frames", "200"]
    owned = frames + ["--set", "hle.movie_lifecycle=on"]
    cases = [
        # the flip-2/3 shape of RootMovieLifecyclePlay: SIGABRT, run.meta written but never finalized
        ("crash exit 134, unfinalized run.meta (owned)", 134, "frames=0\nfinalized=0\n", owned, 1, "FAIL bounded run exits 0"),
        ("crash exit 134, run.meta with video=gl", 134, "frames=64\nfinalized=0\nvideo=gl\n", frames, 1, "FAIL:"),
        ("crash exit 134, no run.meta", 134, None, owned, 1, "FAIL:"),
        ("runner kill 125 (stall)", 125, "frames=30\nfinalized=0\nvideo=gl\n", frames, 1, "FAIL:"),
        ("exit 0, no run.meta", 0, None, frames, 1, "FAIL finalized=1"),
        ("exit 0, finalized=0", 0, "frames=200\nfinalized=0\nvideo=gl\n", frames, 1, "FAIL finalized=1"),
        ("exit 0, finalized but stub video (display-less host)", 0, "frames=200\nfinalized=1\nvideo=stub\n", frames, 77, "SKIP: no window"),
        ("exit 134 with stub video is still a crash", 134, "frames=3\nfinalized=0\nvideo=stub\n", owned, 1, "FAIL:"),
        ("good retained run", 0, GOOD_META, frames, 0, "PASS: play mode presented every movie picture"),
        ("good owned run", 0, GOOD_OWNED_META, owned, 0, "PASS: play mode presented every movie picture"),
        ("good retained run judged as owned", 0, GOOD_META, owned, 1, "FAIL owned movie lifecycle selected"),
    ]
    ok = fake.expect(cases, script)
    print("root_play_fmv_presents selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--timeout", type=int, default=840)
    ap.add_argument("--set", action="append", default=[], dest="settings")
    ap.add_argument("--keep", action="store_true", help="retain the completed NEW run as evidence")
    ap.add_argument("--selftest", action="store_true", help="judge a fake runner (no game process)")
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

    out_dir = tempfile.mkdtemp(prefix="root_play_fmv_presents_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--harness-window", "--harness-out", out_dir, "--frames", str(args.frames),
           "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "120"   # a movie picture can cost > 1 s on a slow host; parks are progress-less
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in ("frames", "finalized", "fmv", "fmv_window", "display_ticks", "fmv_pacer_reanchors", "video", "wall_ms"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if completed_without_window(proc.returncode, meta):
        print("SKIP: no window (video=stub, run completed and finalized): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77

    checks, info = judge(proc.returncode, meta, args.frames, args.settings)
    failures = 0
    for ok, what in checks:
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1
    for line in info:
        print("  " + line)

    if failures:
        print("FAIL: %d check(s) failed (leg kept at %s)" % (failures, out_dir))
        print("artifacts kept: %s %s" % (out_dir, log))
        return 1
    if args.keep:
        print("NEW run retained at %s" % out_dir)
    else:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    print("PASS: play mode presented every movie picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
