#!/usr/bin/env python3
"""RootTitleMusicPlays -- the title music starts under the all-on composition
(hle.music + hle.music_buffers; the owned buffer service, both Movie loans), and starts
WHEN the title expects it: as soon as the stream's first buffer is filled (TR
platform/ps2/multistream.h MsAllowKeyOn: "the stream keys its channel on once its buffers
are full"), not after the complete 4 MiB track window has been read (play session s2:
the music began in the middle of the attract cutscene, audio_first_nonsilent_window=236).

A deterministic headless coldboot `--frames N` through the boot movie to the title
screen must START the title's music pair (parent + interleaved child) through the
owned music service and produce sound. run.meta:

  * frames=N, finalized=1                   -- the cap ended the leg (no stall/abort)
  * music_binding=on, music_buffers_binding=on
                                            -- the pin's premise: the owned music AND
                                               buffer owners are composed (a leg with the
                                               buffers owner off is vacuous, never a pass)
  * music_library=...,plays=<p>,children=<c>,...,starts=<s>,...,errors=0,...,
      refused_window=0,refused_buffers=0,refused_voice=0
                                            -- p > 0, s > 0 and no Play/Child refusal: the
                                               paired child, which the title opens WITHOUT a
                                               buffer of its own (TR game/sound.cpp
                                               CreateMusicBuffers -> filestream.cpp
                                               FileStreamAttachBuffer returns on size 0), is
                                               reserved against its parent's owned storage
  * music_buffers=...,allocations=<a>,...   -- a > 0: the title's buffers went through the
                                               owned service (the parent allocations)
  * audio_first_nonsilent_window=<n>        -- numeric (not `none`): the mixed output was
                                               audible before the cap, and n <= --first-window-max
                                               (default 220: the fixed leg reads 210; the
                                               complete-window charge read 237)
  * music_library ...,fills=<f>,...,first_play_tick=<p>,first_start_tick=<s>
                                            -- f > 0 (the read was charged at the stream's
                                               buffer fill, TR multistream.h: the stream keys
                                               on once its buffers are full), p > 0, s > 0 and
                                               s - p <= --start-ticks-max (default 30 boundary
                                               ticks: the title's buffer is 0x50000 bytes = 160
                                               sectors ~ 148 ms + a long seek 112 ms ~ 16 ticks;
                                               the fixed leg reads 14; the complete 4 MiB window
                                               charge needed >= 120)

Before the fix (all-on build 2026-10-09, int_work/m_series/cutover/allon/leg/run.meta):
`music_library=...,plays=2,children=2,...,starts=0,...,errors=2,...,refused_buffers=2`
and `audio_first_nonsilent_window=none` -- the title music was silent; the --selftest
judges that very run.meta as FAIL.

Verdict rules: FAIL whenever the runner exits non-zero (a crash, SIGABRT 134, a kill by
run_bounded.sh 124/125/137) or run.meta is missing or not finalized=1. SKIP 77 only
without the title files or the Release binary.
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


def counters(text):
    """`a=1,b=2,...` -> {a: 1, b: 2}; a missing or malformed row gives {}."""
    out = {}
    for part in text.split(","):
        m = re.fullmatch(r"([A-Za-z_]+)=(-?\d+)", part.strip())
        if m is None:
            return {}
        out[m.group(1)] = int(m.group(2))
    return out


def judge(returncode, meta, frames, first_window_max=220, start_ticks_max=30):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (the --frames cap ended the leg; frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("music_binding") == "on", "music_binding=on (the owned music service is composed; %s)" % meta.get("music_binding", "<absent>"))
    check(meta.get("music_buffers_binding") == "on",
          "music_buffers_binding=on (the owned buffer service is composed; %s)" % meta.get("music_buffers_binding", "<absent>"))
    music = counters(meta.get("music_library", ""))
    check(music.get("plays", 0) > 0, "music_library plays > 0 (the title requested music; plays=%s)" % music.get("plays", "<absent>"))
    check(music.get("starts", 0) > 0, "music_library starts > 0 (the owned pair started; starts=%s)" % music.get("starts", "<absent>"))
    for key in ("refused_window", "refused_buffers", "refused_voice", "errors"):
        check(key in music and music[key] == 0, "music_library %s=0 (%s)" % (key, music.get(key, "<absent>")))
    buffers = counters(meta.get("music_buffers", ""))
    check(buffers.get("allocations", 0) > 0,
          "music_buffers allocations > 0 (the title's buffers went through the owned service; allocations=%s)" % buffers.get("allocations", "<absent>"))
    first = meta.get("audio_first_nonsilent_window", "")
    check(re.fullmatch(r"\d+", first) is not None, "audio_first_nonsilent_window=<n> (audible before the cap; %s)" % (first or "<absent>"))
    check(re.fullmatch(r"\d+", first) is not None and int(first) <= first_window_max,
          "audio_first_nonsilent_window <= %d (the stream starts at its buffer fill, not after the whole window; %s)"
          % (first_window_max, first or "<absent>"))
    check(music.get("fills", 0) > 0, "music_library fills > 0 (the read was charged at buffer-fill granularity; fills=%s)" % music.get("fills", "<absent>"))
    play, start = music.get("first_play_tick", 0), music.get("first_start_tick", 0)
    check(play > 0 and start > 0 and start >= play and start - play <= start_ticks_max,
          "music_library first_start_tick - first_play_tick <= %d (the pair keys on within its buffer fill's drive time; play=%s start=%s)"
          % (start_ticks_max, music.get("first_play_tick", "<absent>"), music.get("first_start_tick", "<absent>")))
    return checks


# The all-on build's title coldboot before the fix (int_work/m_series/cutover/allon/leg/run.meta,
# 2026-10-09): the paired child refused by the buffer owner, title music silent.
SILENT_META = (
    "frames=1300\nfinalized=1\nmusic_binding=on\nmusic_buffers_binding=on\n"
    "music_library=windows=2,plays=2,children=2,interleaves=2,ends=2,prepares=2,starts=0,stops=0,reads=0,"
    "delivered=0,bytes=0,stale=0,errors=2,refused_window=0,refused_buffers=2,refused_voice=0,fallback=0,statuses=73\n"
    "music_buffers=installed=1,allocations=5,resizes=3,closes=0,queries=3,sound_loans=2,disc_loans=2,returns=4,"
    "errors=0,fallback=0,statuses=79,used=983040,available=1114112,active_loans=0\n"
    "audio_first_nonsilent_window=none\n")
# probe6 (music_buffers=off) played -- but with the buffer owner off the pin is vacuous.
VACUOUS_META = SILENT_META.replace("music_buffers_binding=on", "music_buffers_binding=off").replace(
    "starts=0", "starts=2").replace("errors=2", "errors=0").replace("refused_buffers=2", "refused_buffers=0").replace(
    "allocations=5", "allocations=0").replace("audio_first_nonsilent_window=none", "audio_first_nonsilent_window=237")
# The fixed leg (int_work/m_series/stream/r5/probe8/run.meta, 2026-10-09): the read charged at
# the buffer fill (fills=2), the pair keyed 14 ticks after its Play, audible at window 210.
GOOD_META = (
    "frames=1300\nfinalized=1\nmusic_binding=on\nmusic_buffers_binding=on\n"
    "music_library=windows=2,plays=2,children=2,interleaves=2,ends=2,prepares=2,starts=2,stops=0,reads=2,"
    "delivered=2,bytes=8388608,stale=0,errors=0,refused_window=0,refused_buffers=0,refused_voice=0,fallback=0,statuses=751,"
    "fills=2,refills=3,refill_bytes=393216,first_play_tick=2408,first_start_tick=2422\n"
    "music_buffers=installed=1,allocations=5,resizes=3,closes=0,queries=3,sound_loans=2,disc_loans=2,returns=4,"
    "errors=0,fallback=0,statuses=79,used=983040,available=1114112,active_loans=0\n"
    "audio_first_nonsilent_window=210\n")
# The complete-window charge (r4/root-title-music-plays-diag1: starts=2 but the 4 MiB window
# read as one request before readiness; window 237, the pair keyed ~120 ticks after its Play).
WHOLE_WINDOW_META = GOOD_META.replace("fills=2,refills=3,refill_bytes=393216,first_play_tick=2408,first_start_tick=2422",
                                      "fills=0,refills=0,refill_bytes=0,first_play_tick=2408,first_start_tick=2530").replace(
    "audio_first_nonsilent_window=210", "audio_first_nonsilent_window=237")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    frames = ["--frames", "1300"]
    cases = [
        ("the all-on build before the fix: child refused, silent", 0, SILENT_META, frames, 1, "FAIL music_library starts > 0"),
        ("the all-on build before the fix names the refusal", 0, SILENT_META, frames, 1, "FAIL music_library refused_buffers=0 (2)"),
        ("buffers owner off is vacuous, never a pass", 0, VACUOUS_META, frames, 1, "FAIL music_buffers_binding=on"),
        ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames, 1, "FAIL bounded run exits 0"),
        ("runner kill 125 (stall)", 125, "frames=600\nfinalized=0\n", frames, 1, "RootTitleMusicPlays: FAIL"),
        ("exit 0, no run.meta", 0, None, frames, 1, "FAIL finalized=1"),
        ("good run", 0, GOOD_META, frames, 0, "RootTitleMusicPlays: ALL PASS"),
        ("the complete-window charge: late start", 0, WHOLE_WINDOW_META, frames, 1,
         "FAIL music_library first_start_tick - first_play_tick <= 30"),
        ("the complete-window charge: late window", 0, WHOLE_WINDOW_META, frames, 1,
         "FAIL audio_first_nonsilent_window <= 220"),
        ("the complete-window charge: no fill", 0, WHOLE_WINDOW_META, frames, 1, "FAIL music_library fills > 0"),
        ("good run, wrong frame cap", 0, GOOD_META, ["--frames", "1301"], 1, "FAIL frames=1301"),
        ("good run, no sound", 0, GOOD_META.replace("audio_first_nonsilent_window=210", "audio_first_nonsilent_window=none"),
         frames, 1, "FAIL audio_first_nonsilent_window=<n>"),
    ]
    ok = fake.expect(cases, script)
    print("root_title_music_plays selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=1300)
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable)")
    ap.add_argument("--first-window-max", type=int, default=220,
                    help="latest admissible first non-silent audio window (fixed leg 210; whole-window charge 237)")
    ap.add_argument("--start-ticks-max", type=int, default=30,
                    help="latest admissible first_start_tick - first_play_tick (fixed leg 14; whole-window charge >= 120)")
    ap.add_argument("--keep", action="store_true", help="keep the harness output directory")
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
        print("SKIP: Release binary missing: %s" % args.binary)
        return 77

    out_dir = tempfile.mkdtemp(prefix="root_title_music_")
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
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in ("frames", "finalized", "wall_ms", "music_binding", "music_buffers_binding", "music_library",
                "music_buffers", "audio_first_nonsilent_window", "park", "park_detail"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, args.first_window_max, args.start_ticks_max):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootTitleMusicPlays: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootTitleMusicPlays: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
