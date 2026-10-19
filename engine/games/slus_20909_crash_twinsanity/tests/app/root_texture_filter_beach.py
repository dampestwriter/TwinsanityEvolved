#!/usr/bin/env python3
"""root_texture_filter_beach.py -- RootTextureFilterBeach: trilinear filtering + anisotropy over the
owned mip chains reach the PRESENTED frame of the first level.

Two headless 7600-frame Release runs of the first-level route (RootShadowLevelEntry's padlog:
title -> menu -> the beach; the deterministic fields stated one by one, --profile default, a fresh
memory card per run) on the all-on composition:

  title      the identity arm: the title's own TEX1 choice on every owned draw (census
             video_texture_filter enhanced_draws=0 hw_lod_draws=0).
  trilinear  video.texture_filter=trilinear video.anisotropy=16: every owned draw sampled with
             linear level blending over the owned mip chain and 16x anisotropy under the hardware
             LOD (enhanced_draws>0, hw_lod_draws>0 -- the chains the title's textures carry).

Judged on the presented frame p<PIN_FRAME> (Crash on the beach, the far ground and foliage in the
upper half): the FAR GROUND band differs between the arms -- >= DIFF_SHARE_MIN of its pixels with a
mean |delta| >= DIFF_MEAN_MIN (measured 0.246 / 1.10: the minified sand and foliage resolve more
texel detail under the anisotropic fetch) -- while Crash's body box, a magnified skin texture,
stays within BODY_SHARE_MAX differing pixels (measured 0.008: the same scene, only the sampling
changed). Red-before: with the selection ignored the frames are identical and the band judge fails.

Usage: --binary --game-files --runner [--frames N] [--timeout S] [--keep]; --selftest judges
fake frames / fake run.meta with no game process.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_shadow_level_entry as shadow  # noqa: E402  (the leg mechanics, the ppm reader, counters)

FRAMES = 7600
PIN_FRAME = 7000
PADLOG = shadow.PADLOG
# Top-down logical 512x448 boxes (x0, x1, y0, y1): the far ground + foliage band and Crash's body.
FAR_BOX = (0, 512, 150, 300)
BODY_BOX = shadow.BODY_BOX
DIFF_SHARE_MIN = 0.10     # measured 0.246 (title vs trilinear+16x)
DIFF_MEAN_MIN = 0.4       # measured 1.10
BODY_SHARE_MAX = 0.10     # measured 0.008
ARMS = ("title", "trilinear")
ARM_SETTINGS = {"title": ["video.texture_filter=title", "video.anisotropy=1"],
                "trilinear": ["video.texture_filter=trilinear", "video.anisotropy=16"]}


def box_pixels(path, box):
    w, h, rows = shadow.read_ppm(path, flip=False)   # the presented image is top-down
    x0, x1, y0, y1 = shadow.scaled(box, w / 512.0, h / 448.0)
    return w, h, rows, (x0, x1, y0, y1)


def frame_delta(path_a, path_b, box):
    """(share of differing pixels, mean |delta| per channel) over the box of two presented frames."""
    wa, ha, ra, (x0, x1, y0, y1) = box_pixels(path_a, box)
    wb, hb, rb, _ = box_pixels(path_b, box)
    if (wa, ha) != (wb, hb):
        return None
    n = differing = total = 0
    for y in range(y0, y1):
        row_a, row_b = ra[y], rb[y]
        for x in range(x0, x1):
            i = x * 3
            d = abs(row_a[i] - row_b[i]) + abs(row_a[i + 1] - row_b[i + 1]) + abs(row_a[i + 2] - row_b[i + 2])
            n += 1
            total += d
            if d:
                differing += 1
    return differing / float(max(1, n)), total / float(3 * max(1, n))


def judge_arm(name, returncode, meta, frames, frame_path):
    results = []

    def check(ok, what):
        results.append((bool(ok), "%s: %s" % (name, what)))
        return bool(ok)

    check(returncode == 0, "bounded run exits 0 (exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") == "none", "no park (park=%s)" % meta.get("park", "none"))
    check(meta.get("video") == "gl", "video=gl (video=%s)" % meta.get("video", "<absent>"))
    textures = shadow.counters(meta.get("graphics_textures", ""))
    check(textures.get("loaded", 0) > 0, "graphics_textures loaded>0 (textures at load; %s)"
          % meta.get("graphics_textures", "<absent>")[:60])
    row = meta.get("video_texture_filter", "<absent>")
    f = shadow.counters(row)
    check(row.startswith("filter=%s," % name), "video_texture_filter filter=%s (%s)" % (name, row))
    check(f.get("owned_draws", 0) > 0, "video_texture_filter owned_draws>0 (%s)" % row)
    if name == "title":
        check(f.get("enhanced_draws", 1) == 0 and f.get("hw_lod_draws", 1) == 0,
              "title: enhanced_draws=0 hw_lod_draws=0 (the identity arm; %s)" % row)
    else:
        check(f.get("anisotropy", 0) == 16 and f.get("enhanced_draws", 0) > 0 and f.get("hw_lod_draws", 0) > 0,
              "%s: anisotropy=16 enhanced_draws>0 hw_lod_draws>0 (the owned chains sampled with the hardware LOD; %s)" % (name, row))
    lowering = shadow.counters(meta.get("graphics_kick_lowering", ""))
    check(lowering.get("raw", 1) == 0 and lowering.get("unsupported", 1) == 0,
          "graphics_kick_lowering raw=0 unsupported=0 (every kick of the route lowered natively; %s)"
          % meta.get("graphics_kick_lowering", "<absent>")[:80])
    # The route flushes cloth materials (shader types 0x17 / 0x1A) every frame; their
    # VU1 programs are the library's vs_indexed_displace (broadcast / componentwise), so the
    # route's zero misses above are the cloth draws running native.
    cloth_row = meta.get("graphics_cloth_producer", "<absent>")
    cloth = shadow.counters(cloth_row)
    check(cloth.get("installed", 0) == 1 and cloth.get("type17_packet", 0) > 0 and cloth.get("type1a_packet", 0) > 0,
          "graphics_cloth_producer installed=1 type17_packet>0 type1a_packet>0 (cloth materials flushed on the route; %s)" % cloth_row)
    check(os.path.isfile(frame_path), "presented frame p%d dumped (%s)" % (PIN_FRAME, frame_path))
    return results


def judge_frames(title_path, trilinear_path):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    if not (os.path.isfile(title_path) and os.path.isfile(trilinear_path)):
        check(False, "both presented frames present for the title/trilinear comparison")
        return results
    far = frame_delta(title_path, trilinear_path, FAR_BOX)
    body = frame_delta(title_path, trilinear_path, BODY_BOX)
    if far is None or body is None:
        check(False, "title and trilinear presented frames have one size")
        return results
    share, mean = far
    check(share >= DIFF_SHARE_MIN and mean >= DIFF_MEAN_MIN,
          "presented frame p%d far ground band: trilinear+16x differs from the title's sampling (share %.3f >= %.2f, mean |delta| %.2f >= %.2f)"
          % (PIN_FRAME, share, DIFF_SHARE_MIN, mean, DIFF_MEAN_MIN))
    check(body[0] <= BODY_SHARE_MAX,
          "presented frame p%d body box: the same scene (Crash's magnified skin; differing share %.3f <= %.2f)"
          % (PIN_FRAME, body[0], BODY_SHARE_MAX))
    return results


META_KEYS = ("frames", "finalized", "park", "video", "graphics_textures", "video_texture_filter", "graphics_kick_lowering", "graphics_cloth_producer")


def fake_meta(name, enhanced, hw_lod=0, anisotropy=1, raw=0, cloth17=55250, cloth1a=4604):
    return {"frames": str(FRAMES), "finalized": "1", "park": "none", "video": "gl",
            "graphics_textures": "loaded=1100,destroyed=300,model_calls=0,refused=0,failed=0",
            "graphics_kick_lowering": "submitted=17175902,queued=17175902,lowered=17175902,raw=%d,unsupported=%d" % (raw, raw),
            "graphics_cloth_producer": "installed=1,type17_packet=%d,type17_update=%d,type1a_packet=%d,type1a_update=%d"
                                       % (cloth17, 2 * cloth17, cloth1a, 2 * cloth1a),
            "video_texture_filter": "filter=%s,anisotropy=%d,max_anisotropy=16,owned_draws=14622070,enhanced_draws=%d,hw_lod_draws=%d"
                                    % (name, anisotropy, enhanced, hw_lod)}


def fake_frame(path, far_detail):
    """A beach frame: sky, a far band of 4-px stripes (far_detail: crisp stripes; else their flat
    average -- the title's blurrier minification), sand, Crash's fur in the body box (identical)."""
    def pixel(x, y):
        if y < 150:
            return (120, 170, 230)
        if y < 290:
            if far_detail:
                return (60, 120, 40) if (x // 4) & 1 else (120, 180, 80)
            return (90, 150, 60)
        if 238 <= x < 270 and 290 <= y < 322:
            return (125, 45, 4)
        return (230, 205, 150)
    shadow.write_ppm(path, 512, 448, pixel, flip=False)


def selftest():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="root_texture_filter_beach_selftest_")
    try:
        flat, crisp, flat2 = (os.path.join(tmp, n) for n in ("flat.ppm", "crisp.ppm", "flat2.ppm"))
        fake_frame(flat, False)
        fake_frame(crisp, True)
        shutil.copy(flat, flat2)
        tri = fake_meta("trilinear", 14622070, 14234314, 16)
        cases = [
            ("good title", lambda: judge_arm("title", 0, fake_meta("title", 0), FRAMES, flat), None),
            ("good trilinear", lambda: judge_arm("trilinear", 0, tri, FRAMES, crisp), None),
            ("good frames", lambda: judge_frames(flat, crisp), None),
            # red-before: the selection ignored -- identical frames, no enhanced draw
            ("red-before: identical frames", lambda: judge_frames(flat, flat2), "differs from the title"),
            ("red-before: trilinear enhanced nothing", lambda: judge_arm("trilinear", 0, fake_meta("trilinear", 0, 0, 16), FRAMES, crisp), "enhanced_draws>0"),
            ("no hardware LOD over the chains", lambda: judge_arm("trilinear", 0, fake_meta("trilinear", 5, 0, 16), FRAMES, crisp), "hw_lod_draws>0"),
            ("anisotropy not applied", lambda: judge_arm("trilinear", 0, fake_meta("trilinear", 5, 5, 1), FRAMES, crisp), "anisotropy=16"),
            ("title arm enhanced something", lambda: judge_arm("title", 0, fake_meta("title", 12), FRAMES, flat), "enhanced_draws=0"),
            ("a raw kick on the route", lambda: judge_arm("title", 0, fake_meta("title", 0, raw=3), FRAMES, flat), "raw=0"),
            ("no cloth material flushed", lambda: judge_arm("title", 0, fake_meta("title", 0, cloth17=0), FRAMES, flat), "type17_packet>0"),
            ("no type-1A cloth material flushed", lambda: judge_arm("title", 0, fake_meta("title", 0, cloth1a=0), FRAMES, flat), "type1a_packet>0"),
            ("a crash", lambda: judge_arm("trilinear", 134, dict(tri, finalized="0"), FRAMES, crisp), "exits 0"),
            ("no presented frame", lambda: judge_arm("trilinear", 0, tri, FRAMES, os.path.join(tmp, "missing.ppm")), "dumped"),
            ("a frame missing for the comparison", lambda: judge_frames(flat, os.path.join(tmp, "missing2.ppm")), "both presented frames"),
        ]
        # a frame whose body box moved (another scene) fails the body judge
        moved = os.path.join(tmp, "moved.ppm")
        shadow.write_ppm(moved, 512, 448, lambda x, y: (120, 170, 230) if y < 150 else ((60, 120, 40) if (x // 4) & 1 else (120, 180, 80)) if y < 290 else (230, 205, 150), flip=False)
        cases.append(("the body box changed (another scene)", lambda: judge_frames(flat, moved), "the same scene"))
        for name, run, expect_fail in cases:
            results = run()
            failed = [what for ok, what in results if not ok]
            ok = (not failed) if expect_fail is None else any(expect_fail in what for what in failed)
            print("%s selftest: %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else " -> " + "; ".join(failed or ["(all passed)"])))
            failures += 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootTextureFilterBeachSelftest: %s" % ("PASS" if not failures else "FAIL (%d)" % failures))
    return 1 if failures else 0


def run_leg(args, name, settings, frames_dir, out_dir):
    log = out_dir + ".log"
    os.makedirs(frames_dir, exist_ok=True)
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "default", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", os.path.abspath(PADLOG), "--dump-frames", frames_dir, "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.user_data=" + os.path.join(out_dir, "user_data"), "--set", "video.scale=1"]
    for field in shadow.DETERMINISTIC_FIELDS:
        cmd += ["--set", field]
    for setting in settings:
        cmd += ["--set", setting]
    keeper = shadow.FrameKeeper(frames_dir, (PIN_FRAME,))
    keeper.start()
    print("run[%s]: %s" % (name, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    proc = subprocess.run(cmd, env=env)
    keeper.stop.set()
    keeper.join()
    print("runner[%s] exit=%d" % (name, proc.returncode))
    meta_path = os.path.join(out_dir, "run.meta")
    meta = shadow.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in META_KEYS:
        print("run.meta[%s] %s=%s" % (name, key, meta.get(key, "<absent>")))
    return proc.returncode, meta, log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner")
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.binary or not args.game_files or not args.runner:
        ap.error("--binary, --game-files and --runner are required (or --selftest)")
    if not os.path.isfile(os.path.join(args.game_files, shadow.ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("SKIP: Release binary missing: %s" % args.binary)
        return 77
    if not os.path.isfile(PADLOG):
        print("FAIL: route padlog missing: %s" % PADLOG)
        return 1
    root = tempfile.mkdtemp(prefix="root_texture_filter_beach_")
    failures = 0
    logs = []
    frame_paths = {}
    for name in ARMS:
        out_dir = os.path.join(root, name)
        frames_dir = os.path.join(out_dir, "frames")
        rc, meta, log = run_leg(args, name, ARM_SETTINGS[name], frames_dir, out_dir)
        logs.append(log)
        frame_paths[name] = os.path.join(frames_dir, "p%d.ppm" % PIN_FRAME)
        for ok, what in judge_arm(name, rc, meta, args.frames, frame_paths[name]):
            print("%s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
    for ok, what in judge_frames(frame_paths["title"], frame_paths["trilinear"]):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootTextureFilterBeach: FAIL (%d) -- logs %s, out %s" % (failures, ", ".join(logs), root))
        return 1
    print("RootTextureFilterBeach: ALL PASS")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
