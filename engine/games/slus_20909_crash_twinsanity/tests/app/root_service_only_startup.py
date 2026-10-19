#!/usr/bin/env python3
"""RootServiceOnlyStartup -- a coldboot to the title and its main menu
with the IOP NEVER started: `hle.startup=native`, the compiled all-on default.

TR src/platform/ps2/system.cpp StartServices loads eight IOP drivers (SIO2MAN PADMAN MCMAN
MCSERV LIBSD SDRDRV STREAM CDVDSTM), initialises the IOP heap and the sound remote; the
MultiStream client (stream.cpp / multistream/transport.cpp) then ships a command batch to
the module's RPC server 0x12345 every frame and reads its reply back. Under service-only
startup every one of those requests is answered by a native owner (app/host/native_startup
NativeStartup; the batch by games/<t>/hle/stream StreamTransport, the effects by SoundEffects,
the files/banks/samples/music/buffers by their libraries) -- no IOP kernel, no module rows,
no heartbeat timer, no heap.

A headless coldboot `--frames N` on the RELEASE binary with the deterministic profile's fields
set one by one (the preset itself pins `hle.startup=retained`, the IOP oracle arm for the
acceptance lanes, so the pin names the fields rather than the preset) replays the menu pad
script of RootMenuSfxAudible (START at the title, DOWN/UP x2) and must report:

  * frames=N, finalized=1, exit 0                 -- the cap ended the leg (no stall/abort/park)
  * startup=native, iop_boot=0                    -- the IOP kernel never booted STREAM.IRX
  * iop_modules= (empty), iop_hb_beats=0, stream_rpc_calls=0/0, libsd_calls= (empty)
                                                  -- no module rows, no heartbeat, the model
                                                     server never called, no libsd call
  * service_startup=stream=native,sound=native,retained=none,consumers=0
  * startup_loads=8/0, startup_modules=<8 names>, none refused
  * sif_rpc_calls=<n>/0 with n > 0               -- the title still calls SIF RPC; none unbound
  * stream_transport=installed=1,batches=<b>,...,unowned=0,malformed=0,server_calls=<b>,...
                                                  -- every batch served, every command owned
  * stream_transport_unowned=none
  * iop_heap_native=refused=0,...                 -- the title never needed the IOP heap
  * music_library starts>0 fills>0; sound_output music_nonzero_samples>0   -- music playing
  * RootMenuSfxAudible's judge (plays >= 2, every play started, sample_nonzero_samples > 0)
                                                  -- menu SFX audible

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_menu_sfx_audible as sfx  # noqa: E402  (the menu SFX judge and pad script)

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
# The deterministic preset's fields (engine/core/config/deterministic_profile.cpp) minus the
# preset's `hle.startup=retained` pin: this leg is the native startup under the same clocks.
DETERMINISTIC_FIELDS = (
    "video.scale=1", "video.interp=off", "video.fps_cap=0", "video.vsync=off",
    "cdvd.latency_model=instant", "cdvd.latency_scale=0", "cdvd.mc_latency_polls=0", "cdvd.load_turbo=off",
    "engine.mtvu=off", "engine.pacing=freerun", "engine.time_regime_pin=1",
    "time.tick_sync=on", "time.tick_deterministic=on", "time.t0_tickpaced=on", "time.maint_eepaced=on",
    "iop.dispatch=frame", "game.rtc_fixed=on", "ui.gui_open=off", "dev.log_level=quiet")
MODULES = ("SIO2MAN.IRX", "PADMAN.IRX", "MCMAN.IRX", "MCSERV.IRX", "LIBSD.IRX", "SDRDRV.IRX", "STREAM.IRX", "CDVDSTM.IRX")


def counters(text):
    """`a=1,b=2,...` -> {a: 1, b: 2}; non-integer fields are kept as strings."""
    out = {}
    for part in text.split(","):
        m = re.fullmatch(r"([A-Za-z_0-9]+)=(.*)", part.strip())
        if m is None:
            return {}
        out[m.group(1)] = int(m.group(2)) if re.fullmatch(r"-?\d+", m.group(2)) else m.group(2)
    return out


def judge(returncode, meta, frames, min_plays=2):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (the --frames cap ended the leg; frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") in ("none", ""), "no park (park=%s %s)" % (meta.get("park", "<absent>"), meta.get("park_detail", "")))
    check(meta.get("startup") == "native", "startup=native (hle.startup compiled default; %s)" % meta.get("startup", "<absent>"))
    check(meta.get("iop_boot") == "0", "iop_boot=0 (the IOP kernel never booted STREAM.IRX; %s)" % meta.get("iop_boot", "<absent>"))
    check(meta.get("iop_modules", "<absent>") in ("", "none"), "iop_modules empty (no module rows; %s)" % meta.get("iop_modules", "<absent>")[:60])
    check(meta.get("iop_hb_beats") == "0", "iop_hb_beats=0 (no heartbeat timer; %s)" % meta.get("iop_hb_beats", "<absent>"))
    check(meta.get("stream_rpc_calls") == "0/0", "stream_rpc_calls=0/0 (the model server never called; %s)" % meta.get("stream_rpc_calls", "<absent>"))
    check(meta.get("libsd_calls", "<absent>") in ("", "none"), "libsd_calls empty (no libsd call; %s)" % meta.get("libsd_calls", "<absent>")[:60])
    check(meta.get("service_startup", "").startswith("stream=native,sound=native,retained=none,consumers=0"),
          "service_startup=stream=native,sound=native,retained=none,consumers=0 (%s)" % meta.get("service_startup", "<absent>")[:80])
    check(meta.get("startup_loads") == "8/0", "startup_loads=8/0 (eight sceSifLoadModule answered, none refused; %s)" % meta.get("startup_loads", "<absent>"))
    mods = meta.get("startup_modules", "")
    check(all(("%s:" % m) in mods for m in MODULES) and ":refused" not in mods,
          "startup_modules names every driver with its native owner (%s)" % mods[:160])
    m = re.fullmatch(r"(\d+)/(\d+)", meta.get("sif_rpc_calls", ""))
    check(m is not None and int(m.group(1)) > 0 and int(m.group(2)) == 0,
          "sif_rpc_calls=<n>/0 with n > 0 (the title still calls SIF RPC, none unbound; %s)" % meta.get("sif_rpc_calls", "<absent>"))
    t = counters(meta.get("stream_transport", ""))
    check(t.get("installed") == 1 and t.get("batches", 0) > 0, "stream_transport installed, batches > 0 (%s)" % meta.get("stream_transport", "<absent>")[:120])
    check("unowned" in t and t["unowned"] == 0 and t.get("malformed", -1) == 0,
          "stream_transport unowned=0 malformed=0 (every command of every batch owned; unowned=%s malformed=%s)" % (t.get("unowned", "<absent>"), t.get("malformed", "<absent>")))
    check(t.get("server_calls", -1) == t.get("batches", -2) and t.get("server_malformed", -1) == 0,
          "stream_transport server_calls == batches (every RPC call served; server_calls=%s batches=%s)" % (t.get("server_calls", "<absent>"), t.get("batches", "<absent>")))
    check(t.get("status", 0) > 0 and t.get("registry", 0) >= 6,
          "stream_transport status > 0, registry >= 6 (the title's startup sequence and per-frame status; status=%s registry=%s)" % (t.get("status", "<absent>"), t.get("registry", "<absent>")))
    check(meta.get("stream_transport_unowned") == "none", "stream_transport_unowned=none (%s)" % meta.get("stream_transport_unowned", "<absent>")[:80])
    h = counters(meta.get("iop_heap_native", ""))
    check("refused" in h and h["refused"] == 0, "iop_heap_native refused=0 (the title never needed the IOP heap; %s)" % meta.get("iop_heap_native", "<absent>"))
    music = counters(meta.get("music_library", ""))
    check(music.get("starts", 0) > 0 and music.get("fills", 0) > 0 and music.get("errors", -1) == 0,
          "music_library starts > 0, fills > 0, errors=0 (the title music started; starts=%s fills=%s errors=%s)" % (music.get("starts", "<absent>"), music.get("fills", "<absent>"), music.get("errors", "<absent>")))
    output = counters(meta.get("sound_output", ""))
    check(output.get("music_nonzero_samples", 0) > 0, "sound_output music_nonzero_samples > 0 (music audible; %s)" % output.get("music_nonzero_samples", "<absent>"))
    # The menu SFX judge of RootMenuSfxAudible, as is (plays, started voices, non-zero sample PCM).
    for ok, what in sfx.judge(returncode, meta, frames, min_plays):
        if what.startswith(("sound_", "audio_mix", "sound_output")):
            checks.append((ok, "[RootMenuSfxAudible] " + what))
    return checks


GOOD_META = sfx.GOOD_META + (
    "startup=native\niop_boot=0\niop_modules=\niop_hb_beats=0\nstream_rpc_calls=0/0\nlibsd_calls=\n"
    "service_startup=stream=native,sound=native,retained=none,consumers=0\nstartup_loads=8/0\n"
    "startup_modules=SIO2MAN.IRX:InputBindings,PADMAN.IRX:InputBindings,MCMAN.IRX:SaveStorage,MCSERV.IRX:SaveStorage,"
    "LIBSD.IRX:Audio+SoundEffects,SDRDRV.IRX:SdrRemote+SoundBanks,STREAM.IRX:StreamTransport,CDVDSTM.IRX:FileServices+MovieStream\n"
    "sif_rpc_calls=8400/0\n"
    "stream_transport=installed=1,batches=8399,commands=16900,status=8399,registry=6,global=4,effects=2,compactions=55,movie=0,"
    "recovery=0,wait_updates=0,streams=9,files=36,unowned=0,malformed=0,server_calls=8399,server_malformed=0\n"
    "stream_transport_unowned=none\niop_heap_native=refused=0,frees=0\n"
    "music_library=windows=2,plays=2,children=1,interleaves=1,ends=1,prepares=2,starts=2,stops=0,reads=2,delivered=2,bytes=8000000,"
    "stale=0,errors=0,refused_window=0,refused_buffers=0,refused_voice=0,fallback=0,statuses=1200,fills=2,refills=3,"
    "refill_bytes=393216,first_play_tick=2408,first_start_tick=2422\n")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    frames = ["--frames", "2000"]
    cases = [
        ("good run", 0, GOOD_META, frames, 0, "RootServiceOnlyStartup: ALL PASS"),
        ("the IOP booted (retained startup is never a pass)", 0,
         GOOD_META.replace("startup=native\niop_boot=0\niop_modules=\n", "startup=retained\niop_boot=1\niop_modules=excepman:257,STREAM:256\n"),
         frames, 1, "FAIL startup=native"),
        ("a heartbeat beat", 0, GOOD_META.replace("iop_hb_beats=0", "iop_hb_beats=1900"), frames, 1, "FAIL iop_hb_beats=0"),
        ("the model server called", 0, GOOD_META.replace("stream_rpc_calls=0/0", "stream_rpc_calls=8399/0"), frames, 1, "FAIL stream_rpc_calls=0/0"),
        ("an unowned command in a batch", 0,
         GOOD_META.replace("unowned=0,malformed=0", "unowned=3,malformed=0").replace("stream_transport_unowned=none", "stream_transport_unowned=0x1f:3"),
         frames, 1, "FAIL stream_transport unowned=0"),
        ("a refused module load", 0, GOOD_META.replace("startup_loads=8/0", "startup_loads=7/1").replace("CDVDSTM.IRX:FileServices+MovieStream", "CDVDSTM.IRX:refused"),
         frames, 1, "FAIL startup_loads=8/0"),
        ("a retained consumer", 0, GOOD_META.replace("stream=native,sound=native,retained=none,consumers=0",
                                                     "stream=native,sound=retained,retained=LIBSD.IRX+SDRDRV.IRX,consumers=2"),
         frames, 1, "FAIL service_startup="),
        ("an unbound SIF call", 0, GOOD_META.replace("sif_rpc_calls=8400/0", "sif_rpc_calls=8400/12"), frames, 1, "FAIL sif_rpc_calls=<n>/0"),
        ("a refused heap allocation", 0, GOOD_META.replace("iop_heap_native=refused=0", "iop_heap_native=refused=2"), frames, 1, "FAIL iop_heap_native refused=0"),
        ("silent music", 0, GOOD_META.replace("music_nonzero_samples=849137", "music_nonzero_samples=0"), frames, 1, "FAIL sound_output music_nonzero_samples > 0"),
        ("silent menu SFX (the s2 shape)", 0, GOOD_META.replace("sample_channel_samples=152800,sample_nonzero_samples=31100",
                                                                 "sample_channel_samples=371200,sample_nonzero_samples=0"),
         frames, 1, "FAIL [RootMenuSfxAudible] sound_output sample_nonzero_samples > 0"),
        ("a park", 0, GOOD_META + "park=sif_server_unbound\npark_detail=0x12345\n", frames, 1, "FAIL no park"),
        ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames, 1, "FAIL bounded run exits 0"),
        ("exit 0, no run.meta", 0, None, frames, 1, "FAIL finalized=1"),
    ]
    ok = fake.expect(cases, script)
    print("root_service_only_startup selftest: %s" % ("PASS" if ok else "FAIL"))
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

    out_dir = tempfile.mkdtemp(prefix="root_service_only_startup_")
    log = out_dir + ".log"
    script_path = os.path.join(out_dir, "menu.padlog")
    with open(script_path, "w") as f:
        f.write("\n".join(sfx.SCRIPT) + "\n")
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", script_path]
    for field in DETERMINISTIC_FIELDS:
        cmd += ["--set", field]
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
    meta = sfx.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in ("frames", "finalized", "wall_ms", "startup", "iop_boot", "iop_modules", "iop_hb_beats", "stream_rpc_calls",
                "libsd_calls", "service_startup", "startup_loads", "startup_modules", "sif_rpc_calls", "stream_transport",
                "stream_transport_unowned", "sound_effects", "iop_heap_native", "iop_heap_calls", "music_library", "sound_controls",
                "sound_output", "sound_plays_head", "audio_first_nonsilent_window", "park", "park_detail"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, args.min_plays):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootServiceOnlyStartup: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootServiceOnlyStartup: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
