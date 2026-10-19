#!/usr/bin/env python3
"""RootSpriteTitleLogo -- the title screen's logo (a Sprite: OLEG's titleLogo SpriteWidget
draws sprites[SpriteCrashTitle], the Language\\Titles picture, TR game/oleg.cpp:1049) is drawn
by the OWNED sprite path (hle.graphics_sprites with the widened UI texture selection, TRACK
2b) and is VISIBLE in the PRESENTED frame.

Before this stage the owned path selected only the forty startup HUD icons: every other
Sprite -- the title logo, the menu pictures, the level titles -- ran the generated model
body (`graphics_sprites replaced=0,model_calls=16508` on the 1300-frame coldboot of
int_work/m_series/graphics/track1c/det_after). The census arm of this pin is RED on that
binary. The pixel arm is the s4 lesson (RootShadowLevelEntry): a census that says "owned"
over a presented frame with no logo is a FAIL, so the presenter's OUTPUT (p<key>.ppm, the
window image after the display quad) is judged beside the lane's target grab (f<key>.ppm).

A deterministic headless coldboot leg (the Release binary, `--frames N --dump-frames`,
default N=2520 like RootTitleScreenEffects: the logo is fully faded in at frame 2519) is
judged on:

  run.meta
  * frames=N, finalized=1, park=none, graphics_output_faulted=0
  * graphics_selection sprites=1                      -- the pin's premise
  * graphics_sprites replaced>0, model_calls=0, failed=0
        -- every Sprite::Draw on the route is the owner's (model_calls>0 = a sprite
           producer still on the model: the before arm)
  * graphics_sprites_model_by_texture total=0        -- nothing left by texture
  * graphics_refusals_sprites total=0, faults=0
        -- no selected sprite was refused back to the original or dropped at its slot
  * graphics_textures loaded > 43                     -- pictures beyond the startup icons owned
  pixels (f<key> = the target grab, bottom-up, 512x448 at scale 1; p<key> = the presented
  window image, top-down; the LOGO box (104,110)-(416,236) of RootTitleScreenEffects is
  scaled to each image's size)
  * LOGO: >= 15000 yellow pixels (R>150, R>G>B, B<120; scaled by the image's area) whose
    mean is within +-TOL/channel of the console's (238,216,16) (TOL 8 on the grab, 16 on the
    presented image: the presenter's scaling filter blends the letters' edges)

The --selftest judges fake run.meta/frames: the before census (model_calls=16508) is a red
case, as are a frame without the logo, a presented frame without it (census green), a
refusal at the slot, and a crash/kill.

Verdict rules: FAIL whenever the runner exits non-zero (a crash, SIGABRT 134, a kill by
run_bounded.sh 124/125/137) or run.meta is missing or not finalized=1 or a pin frame was
not dumped. SKIP 77 only without the title files or the Release binary.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
LOGO_BOX = (104, 416, 110, 236)   # x0, x1, y0, y1 (exclusive), top-down, on 512x448
CONSOLE_LOGO = (238, 216, 16)
LOGO_MIN_N = 15000
GRAB_TOL, PRESENTED_TOL = 8, 16
STARTUP_TEXTURES = 43


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
    """`a=1,b=2,x=word,...` -> {a: 1, b: 2}; non-numeric fields are skipped."""
    out = {}
    for part in text.split(","):
        m = re.fullmatch(r"([A-Za-z_]+)=(-?\d+)", part.strip())
        if m is not None:
            out[m.group(1)] = int(m.group(2))
    return out


def read_ppm(path, flip=True):
    """P6 -> (width, height, rows top-down). The target grab f<key> is stored bottom-up
    and flipped here; the presented image p<key> is already top-down."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if m is None:
        raise ValueError("not a P6 ppm: %s" % path)
    w, h, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    pixels = data[m.end():]
    if maxval != 255 or len(pixels) < w * h * 3:
        raise ValueError("ppm payload %s: %dx%d maxval %d, %d bytes" % (path, w, h, maxval, len(pixels)))
    rows = [pixels[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    if flip:
        rows.reverse()
    return w, h, rows


def logo_stat(w, h, rows):
    """(yellow count, yellow mean rgb) over the LOGO box scaled to the image."""
    sx, sy = w / 512.0, h / 448.0
    x0, x1, y0, y1 = int(LOGO_BOX[0] * sx), int(LOGO_BOX[1] * sx), int(LOGO_BOX[2] * sy), int(LOGO_BOX[3] * sy)
    n = sr = sg = sb = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            if r > 150 and r > g > b and b < 120:
                n += 1
                sr += r; sg += g; sb += b
    mean = (sr / n, sg / n, sb / n) if n else (0.0, 0.0, 0.0)
    return n, mean, sx * sy


def judge_frame(key, path, presented=False):
    name = "%s%d" % ("p" if presented else "f", key)
    what = "presented frame" if presented else "frame"
    checks = []
    if not path or not os.path.isfile(path):
        checks.append((False, "%s %s dumped (%s)" % (what, name, path or "<absent>")))
        return checks
    try:
        w, h, rows = read_ppm(path, flip=not presented)
    except Exception as error:  # noqa: BLE001 -- the verdict names the broken dump
        checks.append((False, "%s %s readable (%s)" % (what, name, error)))
        return checks
    if w < 512 or h < 448:
        checks.append((False, "%s %s is at least 512x448 (%dx%d)" % (what, name, w, h)))
        return checks
    n, mean, area = logo_stat(w, h, rows)
    tol = PRESENTED_TOL if presented else GRAB_TOL
    need = int(LOGO_MIN_N * area)
    checks.append((n >= need, "%s %s LOGO visible: %d yellow pixels >= %d (%dx%d)" % (what, name, n, need, w, h)))
    checks.append((n > 0 and all(abs(mean[i] - CONSOLE_LOGO[i]) <= tol for i in range(3)),
                   "%s %s LOGO yellow mean (%.0f,%.0f,%.0f) within +-%d of the console's (238,216,16)"
                   % (what, name, mean[0], mean[1], mean[2], tol)))
    return checks


def judge(returncode, meta, frames, key, frame_paths):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park") == "none", "park=none (park=%s park_detail=%s)" % (meta.get("park", "<absent>"), meta.get("park_detail", "<absent>")))
    check(meta.get("graphics_output_faulted") == "0", "graphics_output_faulted=0 (%s)" % meta.get("graphics_output_faulted", "<absent>"))
    selection = counters(meta.get("graphics_selection", ""))
    check(selection.get("sprites") == 1, "graphics_selection sprites=1 (the owned sprite path is composed; %s)" % (meta.get("graphics_selection") or "<absent>"))
    sprites = counters(meta.get("graphics_sprites", ""))
    check(sprites.get("replaced", 0) > 0, "graphics_sprites replaced > 0 (the route drew sprites through the owner; %s)" % (meta.get("graphics_sprites") or "<absent>"))
    check("model_calls" in sprites and sprites["model_calls"] == 0,
          "graphics_sprites model_calls=0 (no sprite producer on the model; model_calls=%s)" % sprites.get("model_calls", "<absent>"))
    check("failed" in sprites and sprites["failed"] == 0, "graphics_sprites failed=0 (%s)" % sprites.get("failed", "<absent>"))
    left = counters(meta.get("graphics_sprites_model_by_texture", ""))
    check("total" in left and left["total"] == 0,
          "graphics_sprites_model_by_texture total=0 (no sprite left to the original by texture; %s)" % (meta.get("graphics_sprites_model_by_texture") or "<absent>"))
    refusals = counters(meta.get("graphics_refusals_sprites", ""))
    check("total" in refusals and refusals["total"] == 0,
          "graphics_refusals_sprites total=0 (no selected sprite refused or dropped at its slot; %s)" % (meta.get("graphics_refusals_sprites") or "<absent>"))
    check("faults" in refusals and refusals["faults"] == 0, "graphics_refusals_sprites faults=0 (%s)" % refusals.get("faults", "<absent>"))
    textures = counters(meta.get("graphics_textures", ""))
    check(textures.get("loaded", 0) > STARTUP_TEXTURES,
          "graphics_textures loaded > %d (pictures beyond the startup icons owned at load; %s)" % (STARTUP_TEXTURES, meta.get("graphics_textures") or "<absent>"))
    checks.extend(judge_frame(key, frame_paths.get(key)))
    checks.extend(judge_frame(key, frame_paths.get(("p", key)), presented=True))
    return checks


class FrameKeeper(threading.Thread):
    """Deletes every dumped frame except the pin frame while the leg runs."""

    def __init__(self, directory, keep):
        super().__init__(daemon=True)
        self.directory, self.keep, self.stop = directory, set(keep), threading.Event()

    def sweep(self):
        try:
            names = os.listdir(self.directory)
        except OSError:
            return
        for name in names:
            m = re.fullmatch(r"[fp](\d+)(_drawn_0x[0-9a-fA-F]+)?\.ppm", name)
            if m is None:
                continue
            if m.group(2) or int(m.group(1)) not in self.keep:
                try:
                    os.remove(os.path.join(self.directory, name))
                except OSError:
                    pass

    def run(self):
        while not self.stop.is_set():
            self.sweep()
            time.sleep(0.5)
        self.sweep()


def write_ppm(path, w, h, pixel_of, flip=True):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        order = range(h - 1, -1, -1) if flip else range(h)
        for y in order:
            f.write(bytes(c for x in range(w) for c in pixel_of(x, y)))


def fake_frame(path, logo, presented=False, colour=CONSOLE_LOGO):
    """A title-like frame: sky blue with (optionally) the logo's yellow letters as a
    checkerboard over the LOGO box (half the box's pixels = ~19.6k of 39.3k at 512x448)."""
    w, h = (640, 448) if presented else (512, 448)
    sx, sy = w / 512.0, h / 448.0

    def pixel(x, y):
        x, y = x / sx, y / sy
        if logo and LOGO_BOX[0] <= x < LOGO_BOX[1] and LOGO_BOX[2] <= y < LOGO_BOX[3] and (int(x) + int(y)) % 2 == 0:
            return colour
        return (90, 140, 230)
    write_ppm(path, w, h, pixel, flip=not presented)


# The 1300-frame coldboot of the all-on Release before this stage (int_work/m_series/graphics/
# track1c/det_after/run.meta): every title sprite on the model, the forty icons owned.
BEFORE_META = (
    "frames=2520\nfinalized=1\ngraphics_output_faulted=0\ngraphics_selection=sprites=1,text=1,sky=1,movie=1\n"
    "graphics_sprites=replaced=0,model_calls=16508,failed=0,empty=0,refused=0\n"
    "graphics_text=replaced=1139,model_calls=0,failed=0,refused=0\n"
    "graphics_textures=loaded=43,destroyed=0,model_calls=284,failed=0,uploaded=3,restores=0\n"
    "graphics_sprites_model_by_texture=ids=3,total=16508,top=7ea5a0e4:0080:16000,0f6c3a8e:0080:400,00000000:ffff:108\n"
    "graphics_refusals_sprites=total=0,missing_resource=0,unsupported_state=0,ordering=0,faults=0,first=none,last=none,first_fault=none\n"
    "park=none\npark_pc=0x0\npark_detail=frames_cap\n")
GOOD_META = (
    "frames=2520\nfinalized=1\ngraphics_output_faulted=0\ngraphics_selection=sprites=1,text=1,sky=1,movie=1\n"
    "graphics_sprites=replaced=16508,model_calls=0,failed=0,empty=3,refused=0\n"
    "graphics_text=replaced=1139,model_calls=0,failed=0,refused=0\n"
    "graphics_textures=loaded=61,destroyed=8,model_calls=258,failed=0,uploaded=20,restores=0\n"
    "graphics_sprites_model_by_texture=ids=0,total=0,top=none\n"
    "graphics_refusals_sprites=total=0,missing_resource=0,unsupported_state=0,ordering=0,faults=0,first=none,last=none,first_fault=none\n"
    "park=none\npark_pc=0x0\npark_detail=frames_cap\n")
SLOT_META = GOOD_META.replace(
    "graphics_refusals_sprites=total=0,missing_resource=0,unsupported_state=0,ordering=0",
    "graphics_refusals_sprites=total=5566,missing_resource=0,unsupported_state=0,ordering=5566")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    root = tempfile.mkdtemp(prefix="root_sprite_title_logo_selftest_")
    try:
        def frames_dir(name, logo, presented_logo=None, keys=(2519,), presented_keys=(2519,), colour=CONSOLE_LOGO):
            d = os.path.join(root, name)
            os.makedirs(d)
            for key in keys:
                fake_frame(os.path.join(d, "f%d.ppm" % key), logo=logo, colour=colour)
            for key in presented_keys:
                fake_frame(os.path.join(d, "p%d.ppm" % key), logo=logo if presented_logo is None else presented_logo,
                           presented=True, colour=colour)
            return d
        good = frames_dir("good", True)
        blank = frames_dir("blank", False)
        presented_blank = frames_dir("presented_blank", True, presented_logo=False)
        missing = frames_dir("missing", True, keys=())
        no_presented = frames_dir("no_presented", True, presented_keys=())
        wrong_colour = frames_dir("wrong_colour", True, colour=(213, 177, 65))  # the coarsest-mip logo of BUG-084
        frames = ["--frames", "2520", "--key", "2519"]
        cases = [
            ("the before binary: title sprites on the model", 0, BEFORE_META, frames + ["--frames-from", good], 1,
             "FAIL graphics_sprites model_calls=0"),
            ("the before binary: nothing replaced", 0, BEFORE_META, frames + ["--frames-from", good], 1,
             "FAIL graphics_sprites replaced > 0"),
            ("the before binary: the by-texture ledger names the model sprites", 0, BEFORE_META, frames + ["--frames-from", good], 1,
             "FAIL graphics_sprites_model_by_texture total=0"),
            ("the before binary: only the startup icons owned", 0, BEFORE_META, frames + ["--frames-from", good], 1,
             "FAIL graphics_textures loaded > 43"),
            ("owned path off is vacuous", 0, GOOD_META.replace("graphics_selection=sprites=1", "graphics_selection=sprites=0"),
             frames + ["--frames-from", good], 1, "FAIL graphics_selection sprites=1"),
            ("sprites dropped at their slot", 0, SLOT_META, frames + ["--frames-from", good], 1,
             "FAIL graphics_refusals_sprites total=0"),
            ("a ledger fault", 0, GOOD_META.replace("ordering=0,faults=0", "ordering=0,faults=1"), frames + ["--frames-from", good], 1,
             "FAIL graphics_refusals_sprites faults=0"),
            ("a frame without the logo", 0, GOOD_META, frames + ["--frames-from", blank], 1, "FAIL frame f2519 LOGO visible"),
            ("the s4 shape: the grab shows the logo, the PRESENTED frame does not", 0, GOOD_META,
             frames + ["--frames-from", presented_blank], 1, "FAIL presented frame p2519 LOGO visible"),
            ("the logo at the wrong colour (BUG-084's coarsest mip)", 0, GOOD_META, frames + ["--frames-from", wrong_colour], 1,
             "FAIL frame f2519 LOGO yellow mean"),
            ("the pin frame not dumped", 0, GOOD_META, frames + ["--frames-from", missing], 1, "FAIL frame f2519 dumped"),
            ("the presented frame not dumped", 0, GOOD_META, frames + ["--frames-from", no_presented], 1,
             "FAIL presented frame p2519 dumped"),
            ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames + ["--frames-from", good], 1, "FAIL bounded run exits 0"),
            ("runner kill 125 (stall)", 125, "frames=600\nfinalized=0\n", frames + ["--frames-from", good], 1, "RootSpriteTitleLogo: FAIL"),
            ("exit 0, no run.meta", 0, None, frames + ["--frames-from", good], 1, "FAIL finalized=1"),
            ("good run", 0, GOOD_META, frames + ["--frames-from", good], 0, "RootSpriteTitleLogo: ALL PASS"),
            ("good run, wrong frame cap", 0, GOOD_META, ["--frames", "2521", "--key", "2519", "--frames-from", good], 1, "FAIL frames=2521"),
        ]
        ok = fake.expect(cases, script)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("root_sprite_title_logo selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2520)
    ap.add_argument("--key", type=int, default=2519, help="the pin frame (the title logo fully faded in)")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable)")
    ap.add_argument("--frames-from", help="judge the pin frames from this directory instead of dumping (selftest/fake runner)")
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

    out_dir = tempfile.mkdtemp(prefix="root_sprite_title_logo_")
    log = out_dir + ".log"
    frames_dir = args.frames_from or os.path.join(out_dir, "frames")
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--set", "paths.mc_dir=" + mc_dir]
    keeper = None
    if not args.frames_from:
        os.makedirs(frames_dir, exist_ok=True)
        cmd += ["--dump-frames", frames_dir]
        keeper = FrameKeeper(frames_dir, (args.key,))
        keeper.start()
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    proc = subprocess.run(cmd, env=env)
    if keeper is not None:
        keeper.stop.set()
        keeper.join()
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in ("frames", "finalized", "wall_ms", "park", "park_detail", "graphics_output_faulted",
                "graphics_selection", "graphics_sprites", "graphics_sprites_model_by_texture", "graphics_textures",
                "graphics_textures_model_by_texture", "graphics_refusals_sprites", "graphics_producers"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    frame_paths = {args.key: os.path.join(frames_dir, "f%d.ppm" % args.key),
                   ("p", args.key): os.path.join(frames_dir, "p%d.ppm" % args.key)}

    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, args.key, frame_paths):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootSpriteTitleLogo: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootSpriteTitleLogo: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
