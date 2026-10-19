#!/usr/bin/env python3
"""root_texture_filter_title.py -- RootTextureFilterTitle: video.texture_filter reaches the
PRESENTED frame.

Three headless 1000-frame Release coldboots to the title screen on the all-on composition
(the deterministic profile's fields stated one by one so the player's video.* keys apply --
`--profile deterministic` pins video.texture_filter=title AFTER every --set):

  title     the identity arm: the title's own TEX1 choice on every owned draw. The census row
            `video_texture_filter` reads filter=title enhanced_draws=0 hw_lod_draws=0.
  point     every owned draw sampled NEAREST (the title authored bilinear on most of its
            title-screen pictures: enhanced_draws > 0).
  bilinear  every owned draw sampled LINEAR (the title's nearest draws promoted:
            enhanced_draws > 0; the bilinear pictures unchanged).

Judged on the presented frame p<PIN_FRAME> (the title screen with the logo; the pictures are
magnified, so the sampler shows): the LOGO BAND differs between `point` and `bilinear` --
>= DIFF_SHARE_MIN of its pixels differ with a mean |delta| >= DIFF_MEAN_MIN -- and its edges
are STEEPER under point: the share of steep horizontal steps (|delta| > STEEP_STEP per
channel sum) is >= STEEP_RATIO_MIN times the bilinear share. Measured on this binary:
diff share 0.19, mean 1.79, steep 0.0378 vs 0.0295 (ratio 1.28). Red-before: with the
selection ignored the two arms are byte-identical (share 0, ratio 1) and both judges fail.

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

FRAMES = 1000
PIN_FRAME = 900
# Top-down logical 512x448 box (x0, x1, y0, y1): the title logo band (magnified pictures).
LOGO_BOX = (0, 192, 0, 168)
DIFF_SHARE_MIN = 0.05     # measured 0.19 (point vs bilinear)
DIFF_MEAN_MIN = 0.5       # measured 1.79
STEEP_STEP = 48           # per-channel step summed over RGB > 3 * STEEP_STEP = a steep edge
STEEP_RATIO_MIN = 1.10    # measured 1.28 (point steeper than bilinear)
ARMS = ("title", "point", "bilinear")


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


def steep_share(path, box):
    """Share of horizontal neighbour pairs in the box whose RGB step exceeds the steep threshold."""
    w, h, rows, (x0, x1, y0, y1) = box_pixels(path, box)
    n = steep = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1 - 1):
            i = x * 3
            g = abs(row[i] - row[i + 3]) + abs(row[i + 1] - row[i + 4]) + abs(row[i + 2] - row[i + 5])
            n += 1
            if g > 3 * STEEP_STEP:
                steep += 1
    return steep / float(max(1, n))


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
        check(f.get("enhanced_draws", 0) > 0, "%s: enhanced_draws>0 (the selection changed samplers; %s)" % (name, row))
        check(f.get("hw_lod_draws", 1) == 0, "%s: hw_lod_draws=0 (no chain enhancement at anisotropy 1; %s)" % (name, row))
    check(os.path.isfile(frame_path), "presented frame p%d dumped (%s)" % (PIN_FRAME, frame_path))
    return results


def judge_frames(point_path, bilinear_path):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    if not (os.path.isfile(point_path) and os.path.isfile(bilinear_path)):
        check(False, "both presented frames present for the point/bilinear comparison")
        return results
    delta = frame_delta(point_path, bilinear_path, LOGO_BOX)
    if delta is None:
        check(False, "point and bilinear presented frames have one size")
        return results
    share, mean = delta
    check(share >= DIFF_SHARE_MIN and mean >= DIFF_MEAN_MIN,
          "presented frame p%d logo band: point differs from bilinear (share %.3f >= %.2f, mean |delta| %.2f >= %.2f)"
          % (PIN_FRAME, share, DIFF_SHARE_MIN, mean, DIFF_MEAN_MIN))
    sp, sb = steep_share(point_path, LOGO_BOX), steep_share(bilinear_path, LOGO_BOX)
    ratio = sp / sb if sb > 0 else (float("inf") if sp > 0 else 1.0)
    check(ratio >= STEEP_RATIO_MIN,
          "presented frame p%d logo band: point's edges are steeper (steep share %.4f vs bilinear %.4f, ratio %.2f >= %.2f)"
          % (PIN_FRAME, sp, sb, ratio, STEEP_RATIO_MIN))
    return results


META_KEYS = ("frames", "finalized", "park", "video", "graphics_textures", "video_texture_filter")


def fake_meta(name, enhanced, hw_lod=0):
    return {"frames": str(FRAMES), "finalized": "1", "park": "none", "video": "gl",
            "graphics_textures": "loaded=304,destroyed=8,model_calls=0,refused=0,failed=0",
            "video_texture_filter": "filter=%s,anisotropy=1,max_anisotropy=16,owned_draws=8628,enhanced_draws=%d,hw_lod_draws=%d"
                                    % (name, enhanced, hw_lod)}


def fake_frame(path, sharp):
    """A logo band of 8-pixel checker tiles: sharp = hard tile edges (point), soft = a 6-step ramp (bilinear)."""
    def pixel(x, y):
        if x >= 192 or y >= 168:
            return (60, 90, 140)
        tile = ((x // 16) + (y // 16)) & 1
        a, b = (230, 160, 40), (20, 20, 20)
        if sharp:
            return a if tile else b
        # a 6-step ramp into each tile edge (bilinear): no single step is steep
        p = x % 16
        ramp = min(p, 15 - p) / 6.0 if p < 6 or p > 9 else 1.0
        t = 0.5 + 0.5 * ramp if tile else 0.5 - 0.5 * ramp   # tiles meet at the midpoint
        return tuple(int(b[i] + (a[i] - b[i]) * t) for i in range(3))
    shadow.write_ppm(path, 512, 448, pixel, flip=False)


def selftest():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="root_texture_filter_selftest_")
    try:
        sharp, soft, soft2 = (os.path.join(tmp, n) for n in ("sharp.ppm", "soft.ppm", "soft2.ppm"))
        fake_frame(sharp, True)
        fake_frame(soft, False)
        shutil.copy(soft, soft2)
        cases = [
            ("good title", lambda: judge_arm("title", 0, fake_meta("title", 0), FRAMES, sharp), None),
            ("good point", lambda: judge_arm("point", 0, fake_meta("point", 8087), FRAMES, sharp), None),
            ("good bilinear", lambda: judge_arm("bilinear", 0, fake_meta("bilinear", 541), FRAMES, soft), None),
            ("good frames", lambda: judge_frames(sharp, soft), None),
            # red-before: the selection ignored -- identical frames, no enhanced draw
            ("red-before: identical frames", lambda: judge_frames(soft, soft2), "differs from bilinear"),
            ("red-before: point enhanced nothing", lambda: judge_arm("point", 0, fake_meta("point", 0), FRAMES, sharp), "enhanced_draws>0"),
            ("title arm enhanced something", lambda: judge_arm("title", 0, fake_meta("title", 12), FRAMES, sharp), "enhanced_draws=0"),
            ("the census names another mode", lambda: judge_arm("point", 0, fake_meta("bilinear", 5), FRAMES, sharp), "filter=point"),
            ("hardware LOD without anisotropy", lambda: judge_arm("bilinear", 0, fake_meta("bilinear", 5, 3), FRAMES, soft), "hw_lod_draws=0"),
            ("point softer than bilinear", lambda: judge_frames(soft, sharp), "steeper"),
            ("textures at load off", lambda: judge_arm("point", 0, dict(fake_meta("point", 5), graphics_textures="loaded=0,model_calls=300"), FRAMES, sharp), "loaded>0"),
            ("a crash", lambda: judge_arm("point", 134, dict(fake_meta("point", 5), finalized="0"), FRAMES, sharp), "exits 0"),
            ("no presented frame", lambda: judge_arm("point", 0, fake_meta("point", 5), FRAMES, os.path.join(tmp, "missing.ppm")), "dumped"),
            ("a frame missing for the comparison", lambda: judge_frames(sharp, os.path.join(tmp, "missing.ppm")), "both presented frames"),
        ]
        for name, run, expect_fail in cases:
            results = run()
            failed = [what for ok, what in results if not ok]
            ok = (not failed) if expect_fail is None else any(expect_fail in what for what in failed)
            print("%s selftest: %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else " -> " + "; ".join(failed or ["(all passed)"])))
            failures += 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootTextureFilterTitleSelftest: %s" % ("PASS" if not failures else "FAIL (%d)" % failures))
    return 1 if failures else 0


def run_leg(args, name, settings, frames_dir, out_dir):
    log = out_dir + ".log"
    os.makedirs(frames_dir, exist_ok=True)
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "default", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--dump-frames", frames_dir, "--set", "paths.mc_dir=" + mc_dir,
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
    ap.add_argument("--timeout", type=int, default=600)
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
    root = tempfile.mkdtemp(prefix="root_texture_filter_title_")
    failures = 0
    logs = []
    frame_paths = {}
    for name in ARMS:
        out_dir = os.path.join(root, name)
        frames_dir = os.path.join(out_dir, "frames")
        rc, meta, log = run_leg(args, name, ["video.texture_filter=" + name, "video.anisotropy=1"], frames_dir, out_dir)
        logs.append(log)
        frame_paths[name] = os.path.join(frames_dir, "p%d.ppm" % PIN_FRAME)
        for ok, what in judge_arm(name, rc, meta, args.frames, frame_paths[name]):
            print("%s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
    for ok, what in judge_frames(frame_paths["point"], frame_paths["bilinear"]):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootTextureFilterTitle: FAIL (%d) -- logs %s, out %s" % (failures, ", ".join(logs), root))
        return 1
    print("RootTextureFilterTitle: ALL PASS")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
