#!/usr/bin/env python3
"""RootTitleScreenEffects -- the live title-screen gate: a deterministic
headless coldboot leg to the title (the Release binary, `--frames 2520 --dump-frames`) must
present the three effect regions of frame 2519 as the console does (the PCSX2 start_screen
dump, `games/slus_20909_crash_twinsanity/verify/baseline/title_screen/regions.txt`):

  LOGO   the letters' yellow mean within +-8/channel of (238,216,16) and >= 15000 yellow pixels
         (before the fix: (213,177,65), the letters sampled the coarsest mip level);
  ORB    bright share (max channel >= 230) >= 0.35          (console 0.417; before 0.098);
  OCEAN  bright share >= 0.55                               (console 0.659; before 0.472).

The frame is the lane's own image (`<dir>/f2519.ppm`, P6, stored bottom-up -- flipped here by
the same rule `title_regions.py` uses: the first rows are the ocean, blue over red). The
regions and rules are the title gate's (`ORB (196,150)-(316,290)`, `LOGO (104,110)-(416,236)`,
`OCEAN (0,318)-(512,388)`; yellow = R>150, R>G>B, B<120). `judge()` is pure (--selftest).
Exit 77 (SKIP) when the Release binary or the title files are absent.

`--from-probe <image>` (opt-in): resume a frozen title-screen boundary image of the
original tree (`games/slus_20909_crash_twinsanity/verify/baseline/coldboot_attract/probe/old_b2700.ps2st`) instead of the cold
boot and judge the LAST dumped frame (`--frames 6`: seconds instead of ~90 s). Measured on
old_b2700 + 1..6 (Release): LOGO (238,216,14) n=21107 and ORB 0.470 pass the cold-boot bounds,
OCEAN bright 0.455-0.503 does NOT reach the 0.55 bound -- the ocean's specular phase at that
moment of the original tree's timeline is dimmer than at cold-boot frame 2519 (the console's
0.659 is its own moment). The cold boot therefore stays the default of the registered gate;
the probe mode is a fast local check of the LOGO / ORB pins (pass `--ocean-min` to taste --
the registered row's bounds are never lowered).
"""
import re
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
REGIONS = {"ORB": (196, 150, 316, 290), "LOGO": (104, 110, 416, 236), "OCEAN": (0, 318, 512, 388)}
CONSOLE_LOGO = (238, 216, 16)


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


def bottom_up(w, h, px):
    """True when rows 8..31 are blue over red (the ocean on top = stored bottom-up)."""
    r = b = 0
    for y in range(8, min(32, h)):
        row = px[y * w * 3:(y + 1) * w * 3]
        r += sum(row[0::3])
        b += sum(row[2::3])
    return b > r


def region_stats(w, h, px, box, flip):
    """mean, bright share, yellow mean + count over the box (top-down coordinates)."""
    x0, y0, x1, y1 = box
    n = sr = sg = sb = bright = 0
    ys = [0, 0, 0, 0]
    for yy in range(y0, y1):
        y = (h - 1 - yy) if flip else yy
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(x0, x1):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            n += 1
            sr += r; sg += g; sb += b
            if max(r, g, b) >= 230:
                bright += 1
            if r > 150 and r > g > b and b < 120:
                ys[0] += r; ys[1] += g; ys[2] += b; ys[3] += 1
    yn = ys[3]
    return dict(mean=(sr / n, sg / n, sb / n), bright=bright / n,
                yellow=((ys[0] / yn, ys[1] / yn, ys[2] / yn) if yn else (0.0, 0.0, 0.0)), yellow_n=yn)


def judge(stats, logo_tol, logo_min_n, orb_min, ocean_min):
    """(ok, lines) from the three regions' stats (pure; pinned by --selftest)."""
    ok = True
    lines = []
    lg, orb, oc = stats["LOGO"], stats["ORB"], stats["OCEAN"]
    lines.append("regions: ORB bright=%.3f | LOGO yellow=(%.0f,%.0f,%.0f) n=%d bright=%.3f | OCEAN bright=%.3f"
                 % (orb["bright"], lg["yellow"][0], lg["yellow"][1], lg["yellow"][2], lg["yellow_n"], lg["bright"],
                    oc["bright"]))
    lines.append("console: ORB bright=0.417 | LOGO yellow=(238,216,16) n=20501 bright=0.605 | OCEAN bright=0.659")
    if any(abs(lg["yellow"][i] - CONSOLE_LOGO[i]) > logo_tol for i in range(3)):
        ok = False
        lines.append("FAIL: LOGO yellow mean not within +-%d/channel of (238,216,16)" % logo_tol)
    if lg["yellow_n"] < logo_min_n:
        ok = False
        lines.append("FAIL: LOGO yellow pixel count %d < %d" % (lg["yellow_n"], logo_min_n))
    if orb["bright"] < orb_min:
        ok = False
        lines.append("FAIL: ORB bright share %.3f < %.2f" % (orb["bright"], orb_min))
    if oc["bright"] < ocean_min:
        ok = False
        lines.append("FAIL: OCEAN bright share %.3f < %.2f" % (oc["bright"], ocean_min))
    return ok, lines


def selftest():
    good = {"LOGO": dict(mean=(0, 0, 0), bright=0.635, yellow=(238.0, 215.0, 14.0), yellow_n=20792),
            "ORB": dict(mean=(0, 0, 0), bright=0.443, yellow=(0, 0, 0), yellow_n=0),
            "OCEAN": dict(mean=(0, 0, 0), bright=0.701, yellow=(0, 0, 0), yellow_n=0)}
    ok, _ = judge(good, 8, 15000, 0.35, 0.55)
    r = ok
    # The pre-fix live frame: every region red.
    before = {"LOGO": dict(mean=(0, 0, 0), bright=0.182, yellow=(213.0, 177.0, 65.0), yellow_n=23255),
              "ORB": dict(mean=(0, 0, 0), bright=0.098, yellow=(0, 0, 0), yellow_n=0),
              "OCEAN": dict(mean=(0, 0, 0), bright=0.472, yellow=(0, 0, 0), yellow_n=0)}
    ok, lines = judge(before, 8, 15000, 0.35, 0.55)
    r &= (not ok) and sum(1 for l in lines if l.startswith("FAIL")) == 3
    # Synthetic 4x40 frames: 32 blue rows over 8 red = stored bottom-up (the ocean on
    # top); 32 red over 8 blue = top-down. The flipped read of the bottom-up frame's
    # top box sees the red rows.
    w, h = 4, 40
    blue, red = bytes([0, 0, 255] * w), bytes([255, 0, 0] * w)
    up = b"".join([blue] * 32 + [red] * 8)
    down = b"".join([red] * 32 + [blue] * 8)
    r &= bottom_up(w, h, up) is True and bottom_up(w, h, down) is False
    st = region_stats(w, h, up, (0, 0, 4, 8), True)
    r &= st["mean"] == (255.0, 0.0, 0.0) and st["bright"] == 1.0 and st["yellow_n"] == 0
    d = tempfile.mkdtemp(prefix="root_title_selftest_")
    for n in ("f1.ppm", "f3231.ppm", "f3235.ppm", "f3235_drawn_0x70000.ppm", "f9999_drawn_0x70000.ppm"):
        open(os.path.join(d, n), "wb").close()
    r &= last_frame_key(d) == 3235 and last_frame_key(tempfile.mkdtemp(prefix="root_title_empty_")) is None
    shutil.rmtree(d, ignore_errors=True)
    print("root_title_screen_effects selftest: %s" % ("PASS" if r else "FAIL"))
    return 0 if r else 1


def last_frame_key(frames_dir):
    """The highest key among <dir>/f<key>.ppm (the `_drawn_` siblings excluded); None when none."""
    keys = []
    for name in os.listdir(frames_dir):
        m = re.fullmatch(r"f(\d+)\.ppm", name)
        if m:
            keys.append(int(m.group(1)))
    return max(keys) if keys else None


def measure(ppm_path):
    """The three regions' stats of a dumped frame (flipped when stored bottom-up); None if unreadable."""
    img = read_ppm(ppm_path)
    if not img:
        return None
    w, h, px = img
    if (w, h) != (512, 448):
        return None
    flip = bottom_up(w, h, px)
    return {name: region_stats(w, h, px, box, flip) for name, box in REGIONS.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2520)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable; e.g. the "
                         "retained Input/Save/libmc arm whose boot timing this fixed-frame pin was recorded under)")
    ap.add_argument("--key", type=int, help="the judged frame key (cold boot default 2519)")
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--logo-tol", type=int, default=8)
    ap.add_argument("--logo-min-n", type=int, default=15000)
    ap.add_argument("--orb-min", type=float, default=0.35)
    ap.add_argument("--ocean-min", type=float, default=0.55)
    ap.add_argument("--frame", help="judge an existing f<key>.ppm instead of running a leg")
    ap.add_argument("--from-probe", help="resume this frozen boundary image instead of a cold boot; "
                                         "the judged frame is the last dumped one unless --key is given")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.frame:
        st = measure(args.frame)
        if not st:
            print("FAIL: unreadable frame %s" % args.frame)
            return 1
        ok, lines = judge(st, args.logo_tol, args.logo_min_n, args.orb_min, args.ocean_min)
        for ln in lines:
            print(ln)
        print("RootTitleScreenEffects: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    if not (args.binary and args.game_files and args.runner):
        ap.error("--binary, --game-files and --runner are required")
    if not os.path.isfile(args.binary):
        print("SKIP: Release binary absent: %s" % args.binary)
        return 77
    if args.from_probe and not os.path.isfile(args.from_probe):
        print("SKIP: probe image absent: %s" % args.from_probe)
        return 77
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    out_dir = tempfile.mkdtemp(prefix="root_title_screen_effects_")
    frames_dir = os.path.join(out_dir, "frames")
    os.makedirs(frames_dir)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--dump-frames", frames_dir]
    if args.from_probe:
        cmd += ["--load-state", os.path.abspath(args.from_probe)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    rc = 1
    key = args.key if args.key is not None else (last_frame_key(frames_dir) if args.from_probe else 2519)
    frame = os.path.join(frames_dir, "f%d.ppm" % key) if key is not None else os.path.join(frames_dir, "f?.ppm")
    print("judged frame: %s" % frame)
    if proc.returncode != 0:
        print("FAIL: runner exit %d (see %s)" % (proc.returncode, log))
    elif not os.path.isfile(frame):
        print("FAIL: %s missing" % frame)
    else:
        st = measure(frame)
        if not st:
            print("FAIL: unreadable frame %s" % frame)
        else:
            ok, lines = judge(st, args.logo_tol, args.logo_min_n, args.orb_min, args.ocean_min)
            for ln in lines:
                print(ln)
            rc = 0 if ok else 1
    if rc == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
        print("RootTitleScreenEffects: PASS")
    else:
        print("RootTitleScreenEffects: FAIL (leg kept at %s)" % out_dir)
    return rc


if __name__ == "__main__":
    sys.exit(main())
