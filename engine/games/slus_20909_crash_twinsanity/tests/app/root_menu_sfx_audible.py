#!/usr/bin/env python3
"""RootMenuSfxAudible -- a scripted menu navigation under the all-on audio composition
(hle.sound_samples + audio_mix) keys owned sample voices that MIX AUDIBLY.

Play session s2 (all-on Release 966187d1): the menu was silent -- `sound_controls plays=39
errors=0` (every MenuSounds::Play accepted, its owned voice started) but no sample ever mixed:
the title pauses the sound service on the title->menu transition (TR game/gamecontroller.cpp
StopChunks -> PauseGame; game/context.cpp BeginFrame -> PauseSound = MsPauseAll) and keys its
menu samples WHILE paused; the module's pause holds the channels it finds keyed, a channel
keyed afterwards plays -- the owned Audio muted every voice until ResumeAll.

A deterministic headless coldboot `--frames N` on the RELEASE binary replays a pad script
(`--inputs`: START at the title, then DOWN/UP presses in the main menu) and must report:

  * frames=N, finalized=1                   -- the cap ended the leg (no stall/abort)
  * sound_samples_binding=on                -- the pin's premise: the owned sample path is composed
  * audio_mix=selected=on,...               -- ... and the owned output mixes it
  * sound_controls=...,plays=<p>,...,errors=0 -- p >= --min-plays (default 2): the scripted presses
                                               reached MenuSounds::Play through the owned adapter
  * sound_plays_head=b<t>:...,started=1;...  -- every recorded play started an owned voice
  * sound_output=...,sample_channel_samples=<c>,sample_nonzero_samples=<nz>,...
                                            -- c > 0 and nz > 0: the owned SAMPLE voices (not the
                                               music voices) stepped and produced non-zero PCM --
                                               the s2 census shape is c > 0, nz = 0

Before the fix (int_work/m_series/stream/r5/probe4/run.meta: the same script on the
pre-fix build): `sound_controls ... plays=4 ... errors=0`, `sound_output ...,
sample_channel_samples=371200,sample_nonzero_samples=0`; after (r5/probe5):
`sample_nonzero_samples=31100`. The --selftest judges both.

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
# Logical input ticks: START once the title screen is up (the owned boot's title clock
# word advances from ~tick 2400; the title music Play is at 2408 -- RootTitleMusicPlays),
# then four navigation presses once the menu is shown (MenuWidget::Update polls input only
# in StateShown; an appearing menu clears input -- TR game/widgets.cpp:824-844).
SCRIPT = ["# RootMenuSfxAudible: title -> main menu -> DOWN/UP x2 (logical input ticks)",
          "2880-2886:START", "2970-2976:DOWN", "3020-3026:UP", "3070-3076:DOWN", "3120-3126:UP"]


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


def plays_head(text):
    """`b2970:sound=32769,ch=4,...,started=1;b3020:...` -> [{boundary, sound, ..., started}]."""
    out = []
    if not text or text == "none":
        return out
    for record in text.split(";"):
        if ":" not in record:
            return []
        boundary, fields = record.split(":", 1)
        row = {}
        for part in fields.split(","):
            m = re.fullmatch(r"([A-Za-z_]+)=(\d+)(?:/(\d+))?", part.strip())
            if m is None:
                return []
            row[m.group(1)] = int(m.group(2)) if m.group(3) is None else (int(m.group(2)), int(m.group(3)))
        if not row or not re.fullmatch(r"b\d+", boundary):
            return []
        row["boundary"] = int(boundary[1:])
        out.append(row)
    return out


def judge(returncode, meta, frames, min_plays=2):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (the --frames cap ended the leg; frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("sound_samples_binding") == "on",
          "sound_samples_binding=on (the owned sample path is composed; %s)" % meta.get("sound_samples_binding", "<absent>"))
    mix = meta.get("audio_mix", "")
    check(mix.startswith("selected=on,"), "audio_mix selected=on (the owned output mixes the owned voices; %s)" % (mix[:40] or "<absent>"))
    controls = counters(meta.get("sound_controls", ""))
    check(controls.get("plays", 0) >= min_plays,
          "sound_controls plays >= %d (the scripted presses reached MenuSounds::Play; plays=%s)" % (min_plays, controls.get("plays", "<absent>")))
    check("errors" in controls and controls["errors"] == 0, "sound_controls errors=0 (%s)" % controls.get("errors", "<absent>"))
    head = plays_head(meta.get("sound_plays_head", ""))
    check(len(head) > 0 and all(p.get("started") == 1 for p in head),
          "sound_plays_head: every recorded play started an owned voice (%d records, started=%s)"
          % (len(head), [p.get("started") for p in head]))
    output = counters(meta.get("sound_output", ""))
    check(output.get("sample_channel_samples", 0) > 0,
          "sound_output sample_channel_samples > 0 (owned sample voices stepped; %s)" % output.get("sample_channel_samples", "<absent>"))
    check(output.get("sample_nonzero_samples", 0) > 0,
          "sound_output sample_nonzero_samples > 0 (the sample voices mixed audibly, not a held/muted voice; %s)"
          % output.get("sample_nonzero_samples", "<absent>"))
    return checks


# r5/probe4 (pre-fix build, this script): plays accepted and started, every sample stepped silent.
SILENT_META = (
    "frames=2000\nfinalized=1\nsound_samples_binding=on\n"
    "audio_mix=selected=on,ticks=2000,frames=1600000,owned_channel_samples=2728000,unowned_channel_samples=74072000,"
    "movie_frames=0,spu_mix_calls=0\n"
    "sound_output=installed=1,mix_ticks=2000,owned_channel_samples=2728000,sample_channel_samples=371200,"
    "sample_nonzero_samples=0,music_channel_samples=2356800,music_nonzero_samples=849137\n"
    "sound_controls=staged=295,applied=295,plays=4,volume=248,pitch=0,release=0,key_off=0,batches=10176,busy=0,"
    "replies=10176,handoffs=0,shared=6,remaining_volumes=0,fallback=0,errors=0,boundaries=2000\n"
    "sound_plays_head=b2970:sound=32769,ch=4,grp=4,raw=16383/16383,lvl=16383/16383,pitch=1881,atk=32,rel=13,loops=1,"
    "bytes=3072,started=1;b3020:sound=32769,ch=5,grp=4,raw=16383/16383,lvl=16383/16383,pitch=1881,atk=32,rel=13,"
    "loops=1,bytes=3072,started=1\n")
# r5/probe5 (the fixed build, the same script).
GOOD_META = SILENT_META.replace("sample_channel_samples=371200,sample_nonzero_samples=0",
                                "sample_channel_samples=152800,sample_nonzero_samples=31100")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    frames = ["--frames", "2000"]
    cases = [
        ("the s2 shape: plays started, every sample silent", 0, SILENT_META, frames, 1,
         "FAIL sound_output sample_nonzero_samples > 0"),
        ("no plays reached the adapter (the script missed the menu)", 0,
         GOOD_META.replace("plays=4", "plays=0").replace(
             GOOD_META[GOOD_META.index("sound_plays_head="):].rstrip("\n"), "sound_plays_head=none"),
         frames, 1, "FAIL sound_controls plays >= 2"),
        ("a play that did not start its voice", 0, GOOD_META.replace("loops=1,bytes=3072,started=1;", "loops=1,bytes=3072,started=0;"),
         frames, 1, "FAIL sound_plays_head: every recorded play started"),
        ("owned output off is vacuous, never a pass", 0, GOOD_META.replace("audio_mix=selected=on,", "audio_mix=selected=off,"),
         frames, 1, "FAIL audio_mix selected=on"),
        ("sample path off is vacuous, never a pass", 0, GOOD_META.replace("sound_samples_binding=on", "sound_samples_binding=off"),
         frames, 1, "FAIL sound_samples_binding=on"),
        ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames, 1, "FAIL bounded run exits 0"),
        ("runner kill 125 (stall)", 125, "frames=600\nfinalized=0\n", frames, 1, "RootMenuSfxAudible: FAIL"),
        ("exit 0, no run.meta", 0, None, frames, 1, "FAIL finalized=1"),
        ("good run", 0, GOOD_META, frames, 0, "RootMenuSfxAudible: ALL PASS"),
        ("good run, wrong frame cap", 0, GOOD_META, ["--frames", "2001"], 1, "FAIL frames=2001"),
    ]
    ok = fake.expect(cases, script)
    print("root_menu_sfx_audible selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2000)
    ap.add_argument("--timeout", type=int, default=540)
    ap.add_argument("--min-plays", type=int, default=2)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable)")
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

    out_dir = tempfile.mkdtemp(prefix="root_menu_sfx_")
    log = out_dir + ".log"
    script_path = os.path.join(out_dir, "menu.padlog")
    with open(script_path, "w") as f:
        f.write("\n".join(SCRIPT) + "\n")
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", script_path]
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
    for key in ("frames", "finalized", "wall_ms", "sound_samples_binding", "audio_mix", "sound_controls",
                "sound_output", "sound_plays_head", "audio_first_nonsilent_window", "park", "park_detail"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, args.min_plays):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootMenuSfxAudible: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootMenuSfxAudible: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
