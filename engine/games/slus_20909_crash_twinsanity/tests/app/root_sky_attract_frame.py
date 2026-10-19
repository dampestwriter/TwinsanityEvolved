#!/usr/bin/env python3
"""RootSkyAttractFrame -- the OWNED sky pass draws the sky the player sees in the PRESENTED frame.

The bucket-0 sky (TR renderer/sky.cpp:
StartSkyBuffer 0x1b9cf8 / DrawSkyModel 0x1bec10 / FinishSky) is owned under `hle.graphics_sky`.
On every all-on build before this stage the owner REFUSED every frame -- `graphics_refusals_sky
total=3906 missing_resource=3906 producer 0x1b9cf8 detail 4` (SkyPass::Refusal::ModelMissing) --
because the resource it needs, the sky MODEL (a RigidModel the title loads from disk; its parts'
bytes are reached through GetDiskManager's allocation table), was never converted at the load
boundary: under hle.meta_arena the DiskManager's 0x20-byte block descriptors live in the META
arena (0x02000000) and the producers' reader covered main RAM and the scratchpad only, so
SkyAssets::ModelLoaded declined every SkyModelRead and the model path drew the sky instead.

The leg is the deterministic 1300-frame coldboot (the title, then the attract cutscene's opening
"THREE YEARS AGO..." shot: a full-frame sunset sky over the sea from frame ~1260; the sky pass runs
54 times from frame 1246). It dumps the pin frame and judges:

  1. the run finishes (exit 0, finalized, frames=1300, no park, no faulted output);
  2. the census: graphics_selection sky=1; graphics_sky_start/model/finish replaced>0 with
     model_calls=0 and interior_calls=0 (the model bodies never ran); graphics_refusals_sky
     total=0 faults=0; graphics_sky_assets models_loaded>0 textures_loaded>0 faults=0 (the
     load-boundary conversion the pass depends on);
  3. the pin frame: f<key>.ppm (the target grab) and p<key>.ppm (the presenter's output) both
     exist, and the SKY band (SKY_BOX: rows 40-200, the clouds above the horizon; the rows above
     are the cutscene's black letterbox) is a drawn sky -- mean luminance >= SKY_LUM_MIN, blue
     >= SKY_BLUE_MIN and blue >= 0.9 green (not the cleared frame, not a stale target, not sand
     or foliage) --
     and matches the retail sky the
     model arm draws on the same frame within SKY_TOLERANCE per channel (MODEL_SKY, measured on
     the frozen all-on main 1bb648b5: the model sky under the attract shot's colour filter).

Red before: the frozen all-on main 1bb648b5 (`--meta-from` its run.meta) fails the census --
`graphics_sky_start=replaced=0,model_calls=54`, `graphics_refusals_sky total=54 missing_resource=54`,
no graphics_sky_assets row; the model arm of this binary (`--set hle.graphics_sky=off`) fails
selection and model_calls while its frames pass the band checks (the owned sky IS the retail sky:
the two arms' presented frames are byte-identical at the pin frame). The band also caught the
screen owner's broken delivery (every owned filter slot refused): the unfiltered sky read
(170.8, 123.2, 142.4), 13.8 off. `--selftest` judges fake runs.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_shadow_level_entry as shadow  # noqa: E402  (the leg mechanics, the ppm reader, counters)

FRAMES = 1300
PIN_FRAMES = (1290,)
# Top-down logical 512x448 box (x0, x1, y0, y1), exclusive upper bounds, scaled to the image.
SKY_BOX = (32, 480, 40, 200)
# The retail sky the player saw: SKY_BOX mean on p1290 of the frozen all-on main 1bb648b5 (the model
# sky under the model screen filter -- the attract shot runs the chunk's colour filter, which pulls
# the far sky (depth index 0) ~30 % toward the palette's first colour; unfiltered the band reads
# (170.8, 123.2, 142.4)). This stage's Release measures (182.8, 135.7, 128.6) with both owned
# (delta 0.4), (182.5, 135.4, 129.0) with --set hle.graphics_screen=off (the owned sky alone is
# byte-identical to the model sky) and (182.8, 135.7, 128.6) with --set hle.graphics_sky=off.
MODEL_SKY = (182.5, 135.4, 129.0)
SKY_TOLERANCE = 12.0
SKY_LUM_MIN = 100.0
SKY_BLUE_MIN = 110.0
SKY_BLUE_OF_GREEN = 0.9


def box_mean(rows, box, sx, sy):
    x0, x1, y0, y1 = box
    x0, x1 = int(x0 * sx), int(x1 * sx)
    y0, y1 = int(y0 * sy), int(y1 * sy)
    total = [0, 0, 0]
    n = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            total[0] += row[x * 3]
            total[1] += row[x * 3 + 1]
            total[2] += row[x * 3 + 2]
            n += 1
    return tuple(t / float(n) for t in total) if n else (0.0, 0.0, 0.0)


def luminance(mean):
    return 0.299 * mean[0] + 0.587 * mean[1] + 0.114 * mean[2]


def judge_frame(key, path, presented=False):
    kind = "presented frame p%d" % key if presented else "frame f%d" % key
    if not os.path.isfile(path):
        return [(False, "%s dumped" % kind)]
    try:
        w, h, rows = shadow.read_ppm(path, flip=not presented)
    except ValueError as error:
        return [(False, "%s readable (%s)" % (kind, error))]
    mean = box_mean(rows, SKY_BOX, w / 512.0, h / 448.0)
    lum = luminance(mean)
    # A drawn sky: bright, with blue not far under green (the sunset's pink/violet, filtered or
    # not: blue 129 vs green 135 under the filter, 142 vs 123 without); sand (150 vs 205) and
    # foliage are green over blue, the cleared frame is black.
    drawn = lum >= SKY_LUM_MIN and mean[2] >= SKY_BLUE_MIN and mean[2] >= SKY_BLUE_OF_GREEN * mean[1]
    delta = max(abs(a - b) for a, b in zip(mean, MODEL_SKY))
    return [(drawn, "%s sky band mean (%.1f, %.1f, %.1f): a drawn sky, luminance %.1f >= %.0f, blue >= %.0f and blue >= %.1f green (%dx%d)"
             % (kind, mean[0], mean[1], mean[2], lum, SKY_LUM_MIN, SKY_BLUE_MIN, SKY_BLUE_OF_GREEN, w, h)),
            (delta <= SKY_TOLERANCE, "%s sky band within %.0f of the retail sky the model arm draws %s (max channel delta %.1f)"
             % (kind, SKY_TOLERANCE, MODEL_SKY, delta))]


def judge(returncode, meta, frames, frame_paths):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    check(returncode == 0, "bounded run exits 0 (exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta written at the end of the leg)")
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") == "none", "no park (park=%s detail=%s)" % (meta.get("park", "none"), meta.get("park_detail", "")))
    check(meta.get("graphics_output_faulted", "0") == "0", "graphics_output_faulted=0")
    selection = shadow.counters(meta.get("graphics_selection", ""))
    check(selection.get("sky", 0) == 1, "graphics_selection sky=1 (%s)" % meta.get("graphics_selection", "<absent>"))
    for name in ("graphics_sky_start", "graphics_sky_model", "graphics_sky_finish"):
        row = shadow.counters(meta.get(name, ""))
        check(row.get("replaced", 0) > 0 and row.get("model_calls", 1) == 0 and row.get("interior_calls", 1) == 0,
              "%s replaced>0 model_calls=0 interior_calls=0 (%s)" % (name, meta.get(name, "<absent>")))
    refusals = shadow.counters(meta.get("graphics_refusals_sky", ""))
    check(refusals.get("total", 1) == 0 and refusals.get("faults", 1) == 0,
          "graphics_refusals_sky total=0 faults=0 (%s)" % meta.get("graphics_refusals_sky", "<absent>")[:120])
    assets = shadow.counters(meta.get("graphics_sky_assets", ""))
    check(assets.get("models_loaded", 0) > 0 and assets.get("textures_loaded", 0) > 0 and assets.get("faults", 1) == 0,
          "graphics_sky_assets models_loaded>0 textures_loaded>0 faults=0 (%s)" % meta.get("graphics_sky_assets", "<absent>"))
    # allon_w3 (2026-10-12, REVERTED_2a.txt): under textures at load (hle.graphics_textures,
    # 1c-2) TextureBindings::Read writes the ownership word 0xffffffff at object+0x60 where
    # the DiskManager handle was; the sky read it as the handle (-1 -> declines_memory=319,
    # textures_loaded=0, every material refused missing_resource detail 6). The sky now takes
    # those textures from the owned record (TextureBindings::Converted): on a leg where the
    # texture owner converted anything, the sky converted from it and declined none of them.
    textures = shadow.counters(meta.get("graphics_textures", ""))
    if textures.get("loaded", 0) > 0:
        check(assets.get("textures_converted", 0) > 0 and assets.get("declines_converted", 1) == 0,
              "graphics_sky_assets textures_converted>0 declines_converted=0 under textures at load "
              "(graphics_textures loaded=%s; %s)" % (textures.get("loaded"), meta.get("graphics_sky_assets", "<absent>")))
    # allon_w2 (2026-10-11): the owned pass left the sky material's VU1 program (type A,
    # the frame's first load) out of VU1 micro memory, so every later kick ran under an
    # image the title's VU1 library never saw (title leg: vu1_hle misses 0 -> 67; level
    # route: 109 -> 1.15 M, play peak_mb 553 -> 1654). The owned pass now emits the
    # residency (SkyPrograms); the library misses nothing on this leg, as the model arm.
    check(assets.get("programs_uploaded", 0) > 0,
          "graphics_sky_assets programs_uploaded>0 (the sky material's VU1 program residency emitted; %s)"
          % assets.get("programs_uploaded", "<absent>"))
    runs, misses = shadow.vu1_library(meta)
    check(runs > 0 and misses == 0,
          "vu1_hle misses=0 (every kick's micro image is one the title's VU1 library knows, as the model arm's 0; runs=%s misses=%s)"
          % (runs, misses))
    for key in PIN_FRAMES:
        results += judge_frame(key, frame_paths[key])
        results += judge_frame(key, frame_paths[("p", key)], presented=True)
    return results


META_KEYS = ("frames", "finalized", "wall_ms", "park", "park_detail", "graphics_output_faulted", "graphics_selection",
             "graphics_sky_start", "graphics_sky_model", "graphics_sky_finish", "graphics_sky_assets",
             "graphics_refusals_sky", "graphics_textures", "vu1_hle", "vu1_hle_worker", "footprint")

GOOD_META = ("frames=1300\nfinalized=1\ngraphics_output_faulted=0\ngraphics_selection=sprites=1,text=1,sky=1,movie=1\n"
             "graphics_sky_start=replaced=54,model_calls=0,interior_calls=0\n"
             "graphics_sky_model=replaced=432,model_calls=0,interior_calls=0\n"
             "graphics_sky_finish=replaced=54,model_calls=0,interior_calls=0\n"
             "graphics_sky_assets=models_loaded=296,textures_loaded=237,load_declines=429,declines_memory=43,"
             "declines_asset=386,declines_limit=0,declines_converted=0,textures_converted=237,refused=0,"
             "last_refusal=0,faults=0,programs_uploaded=432\n"
             "graphics_textures=loaded=319,destroyed=0,model_calls=258,refused=0,failed=0\n"
             "graphics_refusals_sky=total=0,missing_resource=0,unsupported_state=0,ordering=0,faults=0,first=none,"
             "last=none,first_fault=none\nvu1_hle=20056/0/10\nvu1_hle_worker=0/0/0\n")
# The 2a stage's owned sky before SkyPrograms (lagrange_2c_fix/title_owned/leg.log): no residency row,
# 67 library misses on the title leg (the model arm: 0).
NO_RESIDENCY_META = GOOD_META.replace(",programs_uploaded=432", "").replace("vu1_hle=20056/0/10", "vu1_hle=15543/67/4")
# The frozen all-on main 1bb648b5 (int_work/m_series/graphics/lagrange_2c_fix/sky_title_1300_main): every
# sky start refused ModelMissing; no graphics_sky_assets row existed.
MAIN_META = ("frames=1300\nfinalized=1\ngraphics_output_faulted=0\ngraphics_selection=sprites=1,text=1,sky=1,movie=1\n"
             "graphics_sky_start=replaced=0,model_calls=54,interior_calls=0\n"
             "graphics_sky_model=replaced=0,model_calls=432,interior_calls=0\n"
             "graphics_sky_finish=replaced=0,model_calls=54,interior_calls=0\n"
             "graphics_refusals_sky=total=54,missing_resource=54,unsupported_state=0,ordering=0,faults=0,"
             "first=frame:1246/source_frame:1731/n:1/producer:0x1b9cf8/detail:4/reason:missing_resource,"
             "last=frame:1299/source_frame:1784/n:54/producer:0x1b9cf8/detail:4/reason:missing_resource,first_fault=none\n"
             "vu1_hle=20056/0/10\nvu1_hle_worker=0/0/0\n")
# allon_w3 with 2a in (REVERTED_2a.txt): textures at load on, the sky declined every converted texture.
W3_META = GOOD_META.replace("textures_loaded=237", "textures_loaded=0").replace("declines_memory=43", "declines_memory=319") \
    .replace("textures_converted=237", "textures_converted=0")
MODEL_META = GOOD_META.replace("sky=1", "sky=0").replace("replaced=54,model_calls=0", "replaced=0,model_calls=54") \
    .replace("replaced=432,model_calls=0", "replaced=0,model_calls=432") \
    .replace("models_loaded=296,textures_loaded=237", "models_loaded=0,textures_loaded=0")


def fake_frame(path, colour, presented=False):
    shadow.write_ppm(path, 512, 448, lambda x, y: (0, 0, 0) if y < 32 else colour, flip=not presented)


def selftest():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="root_sky_attract_selftest_")
    try:
        sky = tuple(int(round(c)) for c in MODEL_SKY)
        black = (0, 0, 0)
        shifted = (sky[0] + 20, sky[1], sky[2])   # a drawn sky, but not the retail colour
        sand = (230, 205, 150)                     # bright, not blue: not a sky

        def frames_dir(name, colour, presented_colour=None, keys=PIN_FRAMES, presented_keys=PIN_FRAMES):
            d = os.path.join(tmp, name)
            os.makedirs(d)
            for key in keys:
                fake_frame(os.path.join(d, "f%d.ppm" % key), colour)
            for key in presented_keys:
                fake_frame(os.path.join(d, "p%d.ppm" % key), presented_colour or colour, presented=True)
            return d

        good = frames_dir("good", sky)
        cleared = frames_dir("cleared", black)
        presented_cleared = frames_dir("presented_cleared", sky, presented_colour=black)
        wrong = frames_dir("wrong", shifted)
        not_sky = frames_dir("not_sky", sand)
        missing = frames_dir("missing", sky, keys=())
        no_presented = frames_dir("no_presented", sky, presented_keys=())
        cases = [
            ("the frozen main: every sky start refused ModelMissing", 0, MAIN_META, good, 1,
             "FAIL graphics_sky_start replaced>0 model_calls=0"),
            ("the frozen main: the refusal row", 0, MAIN_META, good, 1, "FAIL graphics_refusals_sky total=0"),
            ("the frozen main: no load census", 0, MAIN_META, good, 1, "FAIL graphics_sky_assets models_loaded>0"),
            ("the model arm: selection", 0, MODEL_META, good, 1, "FAIL graphics_selection sky=1"),
            ("allon_w3: the sky read the ownership word as the handle", 0, W3_META, good, 1,
             "FAIL graphics_sky_assets textures_converted>0 declines_converted=0"),
            ("the 2a stage before SkyPrograms: no residency", 0, NO_RESIDENCY_META, good, 1,
             "FAIL graphics_sky_assets programs_uploaded>0"),
            ("the 2a stage before SkyPrograms: the library misses", 0, NO_RESIDENCY_META, good, 1,
             "FAIL vu1_hle misses=0"),
            ("the model arm: model_calls", 0, MODEL_META, good, 1, "FAIL graphics_sky_model replaced>0 model_calls=0"),
            ("an interior continuation", 0, GOOD_META.replace("replaced=432,model_calls=0,interior_calls=0",
                                                              "replaced=432,model_calls=0,interior_calls=1"), good, 1,
             "FAIL graphics_sky_model replaced>0 model_calls=0 interior_calls=0"),
            ("a fault", 0, GOOD_META.replace("ordering=0,faults=0", "ordering=0,faults=1"), good, 1,
             "FAIL graphics_refusals_sky total=0 faults=0"),
            ("the cleared frame: no sky drawn", 0, GOOD_META, cleared, 1, "FAIL frame f1290 sky band mean (0.0, 0.0, 0.0)"),
            ("a drawn grab beside a cleared presented frame", 0, GOOD_META, presented_cleared, 1,
             "FAIL presented frame p1290 sky band mean (0.0, 0.0, 0.0)"),
            ("a sky of the wrong colour", 0, GOOD_META, wrong, 1, "FAIL frame f1290 sky band within 12 of the retail sky"),
            ("sand where the sky should be", 0, GOOD_META, not_sky, 1, "FAIL frame f1290 sky band mean"),
            ("the pin frame not dumped", 0, GOOD_META, missing, 1, "FAIL frame f1290 dumped"),
            ("the presented frame not dumped", 0, GOOD_META, no_presented, 1, "FAIL presented frame p1290 dumped"),
            ("a short leg", 0, GOOD_META.replace("frames=1300", "frames=1200"), good, 1, "FAIL frames=1300"),
            ("crash exit 134", 134, "frames=0\nfinalized=0\n", good, 1, "FAIL bounded run exits 0"),
            ("good run", 0, GOOD_META, good, 0, "RootSkyAttractFrame: ALL PASS"),
        ]
        for name, code, meta_text, frames, expected_rc, expected_line in cases:
            meta = {}
            for line in meta_text.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    meta[k] = v
            paths = {key: os.path.join(frames, "f%d.ppm" % key) for key in PIN_FRAMES}
            paths.update({("p", key): os.path.join(frames, "p%d.ppm" % key) for key in PIN_FRAMES})
            results = judge(code, meta, FRAMES, paths)
            lines = ["%s %s" % ("PASS" if ok else "FAIL", what) for ok, what in results]
            rc = 0 if all(ok for ok, _ in results) else 1
            lines.append("RootSkyAttractFrame: ALL PASS" if rc == 0 else "RootSkyAttractFrame: FAIL")
            hit = any(line.startswith(expected_line) for line in lines)
            ok = rc == expected_rc and hit
            print("%s selftest: %s" % ("ok  " if ok else "FAIL", name))
            if not ok:
                failures += 1
                for line in lines:
                    print("    " + line)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootSkyAttractFrameSelftest: %s (%d cases)" % ("PASS" if not failures else "FAIL", len(cases)))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable; the red arm is hle.graphics_sky=off)")
    ap.add_argument("--frames-from", help="judge the pin frames from this directory instead of dumping")
    ap.add_argument("--meta-from", help="judge this run.meta instead of running (with --frames-from)")
    ap.add_argument("--keep", action="store_true", help="keep the harness output directory")
    ap.add_argument("--selftest", action="store_true", help="judge fake runs (no game process)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.meta_from:
        meta = shadow.parse_meta(args.meta_from) if os.path.isfile(args.meta_from) else {}
        frames_dir = args.frames_from or ""
        paths = {key: os.path.join(frames_dir, "f%d.ppm" % key) for key in PIN_FRAMES}
        paths.update({("p", key): os.path.join(frames_dir, "p%d.ppm" % key) for key in PIN_FRAMES})
        failures = 0
        for ok, what in judge(0, meta, args.frames, paths):
            print("%s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
        print("RootSkyAttractFrame: %s" % ("ALL PASS" if not failures else "FAIL (%d)" % failures))
        return 1 if failures else 0
    if not args.binary or not args.game_files or not args.runner:
        ap.error("--binary, --game-files and --runner are required (or --selftest / --meta-from)")
    if not os.path.isfile(os.path.join(args.game_files, shadow.ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("SKIP: Release binary missing: %s" % args.binary)
        return 77

    out_dir = tempfile.mkdtemp(prefix="root_sky_attract_frame_")
    log = out_dir + ".log"
    frames_dir = os.path.join(out_dir, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--dump-frames", frames_dir]
    for setting in args.settings:
        cmd += ["--set", setting]
    keeper = shadow.FrameKeeper(frames_dir, PIN_FRAMES)
    keeper.start()
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    proc = subprocess.run(cmd, env=env)
    keeper.stop.set()
    keeper.join()
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(out_dir, "run.meta")
    meta = shadow.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in META_KEYS:
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    paths = {key: os.path.join(frames_dir, "f%d.ppm" % key) for key in PIN_FRAMES}
    paths.update({("p", key): os.path.join(frames_dir, "p%d.ppm" % key) for key in PIN_FRAMES})
    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, paths):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootSkyAttractFrame: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootSkyAttractFrame: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
