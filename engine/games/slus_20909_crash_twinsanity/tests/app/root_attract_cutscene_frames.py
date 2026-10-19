#!/usr/bin/env python3
"""RootAttractCutsceneFrames -- the ground-truth frame gate of the in-engine "THREE YEARS AGO..."
cutscene (the `coldboot_attract` leg's first pass) against the console: the PCSX2 GS dumps'
embedded screenshots under `games/slus_20909_crash_twinsanity/verify/baseline/coldboot_attract/gsdumps/`.

A deterministic headless cold boot of the Release binary (`--frames 2350 --dump-frames`) must
show, for each of the four cutscene shots, a frame inside a +-25 window around the shot's
NEW frame whose six region means (SKY / HORIZON / WATER / CAPTION / RIGHT / LEFT, 512x448
coordinates) are each within an ABSOLUTE per-channel tolerance of the console's, and whose
CAPTION box carries the caption's letters (bright share). The window is the pairing
uncertainty (PCSX2's +-15 frames on the cutscene start A, plus NEW's pass running ~20 frames
shorter than the console's; the best frame in it is the one with the least
summed |delta| over the 18 channel means, and THAT frame is judged.

Tolerance rationale (measured at HEAD 64ba49b). The bound is
ABSOLUTE, not relative: the CAPTION region is near-black ((7,5,6) on the console, (7,4,3) on
NEW), where +-10 % relative is 0.5 of a level and fails on the console-faithful frame. The
measured residual at the best frames is max |delta| 4.6 / 4.7 / 4.5 / 5.2 per channel (SKY R
on three shots: NEW (81,59,64) vs the console's (77,55,61)); no pre-fix frame exists, so the
bound is 12 = twice the residual (the title gate's shape: `root_title_screen_effects.py
--logo-tol 8` over its measured <= 5). RED is measured: the windows shifted +100 / -100 /
+432 frames (a wrong frame / another shot's frame) exceed 12 by 13-89 on at least two shots
each (mid1's iceberg scene is nearly static: 75 frames early it is still within 8.1 -- the
gate is a content gate, not a timing gate). The means alone would NOT see a missing caption
((0,0,0) vs (7,5,6) is inside 12): the CAPTION bright share (pixels with a channel >= 128;
console 0.046, NEW 0.034 on every shot) must be >= 0.02, midway between a skipped draw (0)
and NEW's measured value -- a frame with its caption blanked is RED (measured).

The console shot is 640x480 top-down (the dump's embedded screenshot); its region means are
taken over the boxes scaled by 640/512 x 480/448 -- a region mean does not depend on a
resampling filter. NEW's `--dump-frames` PPMs are 512x448 and stored bottom-up (the first rows
are the ocean); they are flipped by the same rule `title_regions.py` and the title gate use
(`--stored-top-down` turns it off). `title_return_f4200` is not pinned here: it is the title
screen, pinned live against the console's start_screen dump by RootTitleScreenEffects.

`judge()` is pure (--selftest). `--frames-dir D [--shift N]` judges an existing dump directory
(a shift of the window centres is the RED probe: a wrong-frame window). Exit 77 (SKIP) when
the Release binary, the title files or the shots are absent. The leg's PPMs outside the
windows (and the `_drawn_` siblings) are pruned while the leg runs: a 2350-frame dump is
~2.9 GB whole, ~140 MB kept.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
REGIONS = {"SKY": (0, 0, 512, 110), "HORIZON": (0, 110, 512, 200), "WATER": (0, 200, 512, 330),
           "CAPTION": (64, 380, 448, 436), "RIGHT": (384, 100, 512, 330), "LEFT": (0, 100, 128, 330)}
# shot name -> (screenshot file, the NEW frame at the window's centre). The centres follow the
# cold boot's scene edge (the boot state machine's case-7 boundary): measured
# 1233 / 1665 / 1951 / 2299 at HEAD 64ba49b (edge new_b1133; the edge-derived boundaries read
# 1218 / 1673 / 1973 / 2318), and 85 frames earlier -- 1148 / 1580 / 1866 / 2214, the best frames
# landing exactly on the shifted centres with the same residuals (4.6 / 4.6 / 4.5 / 5.2) -- since
# the loader's ring rule (loader.ring_cache, default on) moved the edge to new_b1048.
SHOTS = {"attract_start": ("attract_start_f2845_shot.ppm", 1148),
         "attract_mid1": ("attract_mid1_f3300_shot.ppm", 1580),
         "attract_mid2": ("attract_mid2_f3600_shot.ppm", 1866),
         "attract_end": ("attract_end_f3945_shot.ppm", 2214)}
CHANNELS = ("R", "G", "B")


def read_ppm(path):
    """(w, h, bytes) of a binary P6 PPM; None when malformed."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or parts[0].strip() != b"P6":
        return None
    w, h = (int(x) for x in parts[1].split())
    px = parts[3]
    if len(px) < w * h * 3:
        return None
    return w, h, px


def region_mean(w, h, px, box, flip, scale=(1.0, 1.0)):
    """Mean (r, g, b) over the box given in 512x448 top-down coordinates, scaled to the image's
    size; `flip` reads the rows bottom-up (the stored order of NEW's --dump-frames PPMs)."""
    sx, sy = scale
    x0, x1 = int(round(box[0] * sx)), int(round(box[2] * sx))
    y0, y1 = int(round(box[1] * sy)), int(round(box[3] * sy))
    sr = sg = sb = 0
    for yy in range(y0, y1):
        y = (h - 1 - yy) if flip else yy
        row = px[y * w * 3 + x0 * 3:y * w * 3 + x1 * 3]
        sr += sum(row[0::3])
        sg += sum(row[1::3])
        sb += sum(row[2::3])
    n = (y1 - y0) * (x1 - x0)
    return (sr / n, sg / n, sb / n)


def region_means(img, flip):
    """{region: (r, g, b)} of a frame of any size (the boxes scaled from 512x448)."""
    w, h, px = img
    scale = (w / 512.0, h / 448.0)
    return {name: region_mean(w, h, px, box, flip, scale) for name, box in REGIONS.items()}


def caption_bright(img, flip):
    """Share of the CAPTION box's pixels with a channel >= 128: the caption's letters (the
    console 0.046, NEW 0.034 at every shot; a frame without the caption draw reads 0)."""
    w, h, px = img
    sx, sy = w / 512.0, h / 448.0
    box = REGIONS["CAPTION"]
    x0, x1 = int(round(box[0] * sx)), int(round(box[2] * sx))
    y0, y1 = int(round(box[1] * sy)), int(round(box[3] * sy))
    n = bright = 0
    for yy in range(y0, y1):
        y = (h - 1 - yy) if flip else yy
        row = px[y * w * 3 + x0 * 3:y * w * 3 + x1 * 3]
        n += (x1 - x0)
        bright += sum(1 for i in range(0, len(row), 3) if row[i] >= 128 or row[i + 1] >= 128 or row[i + 2] >= 128)
    return bright / n


def measure(img, flip):
    """(region means, caption bright share) of one frame."""
    return region_means(img, flip), caption_bright(img, flip)


def deltas(means, ref):
    """(sum of |delta| over the 18 channel means, max |delta|, (region, channel) of the max)."""
    total = 0.0
    worst = (0.0, None)
    for name in REGIONS:
        for i in range(3):
            d = abs(means[name][i] - ref[name][i])
            total += d
            if d > worst[0]:
                worst = (d, (name, CHANNELS[i]))
    return total, worst[0], worst[1]


def fmt(m):
    return "(%d,%d,%d)" % tuple(int(round(v)) for v in m)


def judge(shot, ref, frames, centre, radius, tol, caption_min):
    """(ok, lines) for one shot: `frames` maps frame key -> (region means, caption bright share)
    (the window's frames; missing keys are absent), `ref` is the console shot's pair. The best
    frame (least summed |delta| of the means) is judged channel by channel against the absolute
    tolerance, and its caption bright share against `caption_min`. Pure."""
    lines = []
    best = None
    for key in range(centre - radius, centre + radius + 1):
        fr = frames.get(key)
        if fr is None:
            continue
        total, worst, where = deltas(fr[0], ref[0])
        if best is None or total < best[0]:
            best = (total, key, fr, worst, where)
    if best is None:
        lines.append("FAIL: %s: no frame in [%d, %d]" % (shot, centre - radius, centre + radius))
        return False, lines
    total, key, (m, bright), worst, where = best
    lines.append("%s: best f%d (centre %d %+d) sum|delta|=%.1f max|delta|=%.1f at %s/%s caption bright=%.3f" %
                 (shot, key, centre, key - centre, total, worst, where[0], where[1], bright))
    lines.append("  NEW     " + " ".join("%s=%s" % (n, fmt(m[n])) for n in REGIONS))
    lines.append("  console " + " ".join("%s=%s" % (n, fmt(ref[0][n])) for n in REGIONS) +
                 " caption bright=%.3f" % ref[1])
    ok = True
    for name in REGIONS:
        for i in range(3):
            d = abs(m[name][i] - ref[0][name][i])
            if d > tol:
                ok = False
                lines.append("FAIL: %s: %s %s mean %.1f vs console %.1f (|delta| %.1f > %d)" %
                             (shot, name, CHANNELS[i], m[name][i], ref[0][name][i], d, tol))
    if bright < caption_min:
        ok = False
        lines.append("FAIL: %s: CAPTION bright share %.3f < %.3f (the caption's letters missing)" %
                     (shot, bright, caption_min))
    return ok, lines


def selftest():
    ref = {"SKY": (76, 55, 61), "HORIZON": (225, 169, 149), "WATER": (63, 107, 203),
           "CAPTION": (7, 5, 6), "RIGHT": (137, 138, 177), "LEFT": (127, 134, 189)}
    near = {"SKY": (81, 59, 64), "HORIZON": (227, 173, 151), "WATER": (60, 105, 204),
            "CAPTION": (7, 4, 3), "RIGHT": (135, 138, 179), "LEFT": (128, 135, 190)}
    far = dict(near, WATER=(60, 105, 230))          # one channel 27 off: a wrong frame
    cref, cnew = (ref, 0.046), (near, 0.034)
    r = True
    # GREEN: the measured attract_start best frame (max |delta| 5) inside tolerance 12; the
    # best frame is chosen by the summed |delta| (f1233 over the farther f1206).
    ok, lines = judge("attract_start", cref, {1206: (far, 0.034), 1233: cnew}, 1218, 25, 12, 0.02)
    r &= ok and lines[0].startswith("attract_start: best f1233 (centre 1218 +15)")
    # RED: only the far frame in the window -> one FAIL line naming WATER/B.
    ok, lines = judge("attract_start", cref, {1206: (far, 0.034)}, 1218, 25, 12, 0.02)
    r &= (not ok) and sum(1 for l in lines if l.startswith("FAIL")) == 1 and "WATER B" in lines[-1]
    # RED: the near frame outside the window -> no frame.
    ok, lines = judge("attract_start", cref, {1300: cnew}, 1218, 25, 12, 0.02)
    r &= (not ok) and lines[0].startswith("FAIL: attract_start: no frame in [1193, 1243]")
    # RED: the caption's letters missing (a skipped draw): the means are inside the bound
    # ((0,0,0) vs (7,5,6)), the bright share is not.
    ok, lines = judge("attract_start", cref, {1233: (dict(near, CAPTION=(0, 0, 0)), 0.0)}, 1218, 25, 12, 0.02)
    r &= (not ok) and sum(1 for l in lines if l.startswith("FAIL")) == 1 and "CAPTION bright share 0.000" in lines[-1]
    # A relative +-10 % rule would be RED on CAPTION (7,4,3) vs (7,5,6); the absolute rule is not.
    ok, _ = judge("caption", cref, {1: cnew}, 1, 0, 6, 0.02)
    r &= ok
    # Synthetic 4x40 frame: 32 blue rows over 8 red = stored bottom-up; the flipped read of the
    # top box (rows 0..8 top-down) sees the red rows; the unflipped read sees blue.
    w, h = 4, 40
    blue, red = bytes([0, 0, 255] * w), bytes([255, 0, 0] * w)
    up = b"".join([blue] * 32 + [red] * 8)
    r &= region_mean(w, h, up, (0, 0, 4, 8), True) == (255.0, 0.0, 0.0)
    r &= region_mean(w, h, up, (0, 0, 4, 8), False) == (0.0, 0.0, 255.0)
    # The scaled box: a 640x480 image's box (0,0,512,448) is the whole image.
    w2, h2 = 640, 480
    img = (w2, h2, bytes([10, 20, 30] * (w2 * h2)))
    r &= region_mean(w2, h2, img[2], (0, 0, 512, 448), False, (1.25, 480 / 448.0)) == (10.0, 20.0, 30.0)
    print("root_attract_cutscene_frames selftest: %s" % ("PASS" if r else "FAIL"))
    return 0 if r else 1


def shot_means(shots_dir):
    """{shot: console region means} from the dumps' embedded screenshots; None if one is missing."""
    out = {}
    for shot, (fname, _) in SHOTS.items():
        img = read_ppm(os.path.join(shots_dir, fname))
        if not img:
            return None
        out[shot] = measure(img, False)
    return out


def windows(radius, shift):
    """{shot: (centre, radius)} with the centres shifted (the RED probe)."""
    return {shot: (centre + shift, radius) for shot, (_, centre) in SHOTS.items()}


def frame_keys(wins):
    keys = set()
    for centre, radius in wins.values():
        keys.update(range(centre - radius, centre + radius + 1))
    return keys


def judge_dir(frames_dir, shots_dir, wins, tol, caption_min, flip):
    """(rc, lines): every shot judged on the frames under `frames_dir`."""
    refs = shot_means(shots_dir)
    if refs is None:
        return 77, ["SKIP: shots absent under %s" % shots_dir]
    lines = []
    ok_all = True
    for shot in SHOTS:
        centre, radius = wins[shot]
        frames = {}
        for key in range(centre - radius, centre + radius + 1):
            p = os.path.join(frames_dir, "f%d.ppm" % key)
            if not os.path.isfile(p):
                continue
            img = read_ppm(p)
            if img and (img[0], img[1]) == (512, 448):
                frames[key] = measure(img, flip)
        ok, out = judge(shot, refs[shot], frames, centre, radius, tol, caption_min)
        ok_all &= ok
        lines.extend(out)
    return (0 if ok_all else 1), lines


def prune(frames_dir, keep):
    """Delete the dump's PPMs whose key is not kept (and every `_drawn_` sibling)."""
    for name in os.listdir(frames_dir):
        if not name.endswith(".ppm"):
            continue
        stem = name[1:-4]
        if "_drawn_" in stem or not stem.isdigit() or int(stem) not in keep:
            try:
                os.remove(os.path.join(frames_dir, name))
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--shots", help="games/slus_20909_crash_twinsanity/verify/baseline/coldboot_attract/gsdumps")
    ap.add_argument("--frames", type=int, default=2350)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable; e.g. the "
                         "retained Input/Save/libmc arm whose boot timing this fixed-frame pin was recorded under)")
    ap.add_argument("--radius", type=int, default=25)
    ap.add_argument("--tol", type=int, default=12, help="absolute per-channel bound on the region means")
    ap.add_argument("--caption-min", type=float, default=0.02,
                    help="least CAPTION bright share (console 0.046, NEW 0.034; a skipped caption draw 0)")
    ap.add_argument("--shift", type=int, default=0, help="shift every window centre (the RED probe)")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--stored-top-down", action="store_true", help="do not flip the dumped frames")
    ap.add_argument("--frames-dir", help="judge an existing --dump-frames directory instead of running a leg")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    flip = not args.stored_top_down
    wins = windows(args.radius, args.shift)
    if args.frames_dir:
        if not args.shots:
            ap.error("--shots is required with --frames-dir")
        rc, lines = judge_dir(args.frames_dir, args.shots, wins, args.tol, args.caption_min, flip)
        for ln in lines:
            print(ln)
        print("RootAttractCutsceneFrames: %s" % {0: "PASS", 1: "FAIL", 77: "SKIP"}[rc])
        return rc
    if not (args.binary and args.game_files and args.runner and args.shots):
        ap.error("--binary, --game-files, --runner and --shots are required")
    if not os.path.isfile(args.binary):
        print("SKIP: Release binary absent: %s" % args.binary)
        return 77
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if shot_means(args.shots) is None:
        print("SKIP: shots absent under %s" % args.shots)
        return 77
    out_dir = tempfile.mkdtemp(prefix="root_attract_cutscene_frames_")
    frames_dir = os.path.join(out_dir, "frames")
    os.makedirs(frames_dir)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--dump-frames", frames_dir]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "120"
    keep = frame_keys(wins)
    proc = subprocess.Popen(cmd, env=env)
    while proc.poll() is None:
        time.sleep(2)
        prune(frames_dir, keep)
    prune(frames_dir, keep)
    rc = 1
    if proc.returncode != 0:
        print("FAIL: runner exit %d (see %s)" % (proc.returncode, log))
    else:
        rc, lines = judge_dir(frames_dir, args.shots, wins, args.tol, args.caption_min, flip)
        for ln in lines:
            print(ln)
    if rc == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
        print("RootAttractCutsceneFrames: PASS")
    else:
        print("RootAttractCutsceneFrames: FAIL (leg kept at %s)" % out_dir)
    return rc


if __name__ == "__main__":
    sys.exit(main())
