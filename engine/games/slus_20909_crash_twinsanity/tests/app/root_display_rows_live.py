#!/usr/bin/env python3
"""RootDisplayRowsLive: two Display-page rows changed from the title's OWN menu
take effect LIVE in the running session and persist -- the internal scale (video.scale, the
ChoiceItem stepped 1x -> 2x with RIGHT) and vsync (video.vsync, the ToggleItem pressed with CROSS).

One play leg (`--harness-window`, the default profile, `--inputs` = tests/app/root_display_rows_live.padlog:
title -> START -> main menu -> options -> the added 'display' link -> the Display page -> DOWN x2 ->
RIGHT on 'internal scale' at frame 3420 -> DOWN -> CROSS on 'vsync' at 3540; `--frames 3700`), with
`--dump-frames <dir>` (the session's LAST window image w_final.ppm: what the user sees) and
`--set paths.user_data=<tmp>` (the settings file lands in the leg's own root).

Judged (every line read from run.meta / the settings file / the window image):
  * runner exit 0, run.meta finalized=1 frames=N park=none video=gl, menu_ext state=registered
    pages=2 items=8 texts=35 (the integrated composition: the A2 filter rows join the page).
  * menu_ext changes=2 (the two user changes applied from the title's menu), menu_ext_pages
    video.scale=2 and video.vsync=1 (the items hold the new values).
  * THE SCALE TOOK EFFECT: display_rows scale_setting=2 scale=2 (VideoStack::Scale() -- the
    ScalePolicy factor every owned pass reads), scale_requests=1 scale_applied=1 (ONE ordered job on
    the GS consumer; scale_async=1 in play: the game thread never waited -- the clock contract's
    rule 2), and grab=1024x896 -- the consumer's latest display grab, the frame the presenter shows
    at the end of the leg, is at scale 2 (512x448 x 2); gfx_consumer resizes >= 1 (the targets
    re-created at the new size).
  * VSYNC TOOK EFFECT: display_rows vsync=1 (SessionWindow::VSync after SDL_GL_SetSwapInterval(1)
    was accepted).
  * the PAGE STILL RENDERS after both changes: w_final has >= MENU_MIN title-font pixels in the items
    box (the scale change re-created the targets without losing the frame).
  * <root>/ps2r.ini carries `video.scale = 2` and `video.vsync = on`.

Red-before: a binary without DisplayRows leaves scale_requests=0 / grab=512x448 / no ps2r.ini key
(the menu item changes, nothing follows). --selftest judges fake inputs: an unapplied scale (the
request taken, nothing applied, the grab still 512x448), a vsync refused, a crash after the step,
a leg whose settings file lacks a key, a frame without the menu are red cases.
SKIP 77 only without the title files / the binary, or when the window is a stub (video=stub).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
MENU_BOX = (0.30, 0.68, 0.22, 0.60)   # x0,x1,y0,y1 as fractions of the 4:3 PICTURE: the items ellipse
MENU_MIN = 600
ITEMS = 8     # Display 7 (fps overlay, display mode, scale, vsync, aspect, texture filter, anisotropic;
              # video.render_rate PARKED/hidden
TEXTS = 35    # MenuStrings::kCount (24 + the 11 texture filter / anisotropy strings)
SCALE_AFTER = 2
GRAB_AFTER = "1024x896"


def parse_meta(path):
    meta = {}
    if not os.path.isfile(path):
        return meta
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    return meta


def census(text):
    out = {}
    for part in (text or "").split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def read_ppm(path):
    """P6 -> (w, h, bytes) or None."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    if len(px) < w * h * 3:
        return None
    return w, h, px


def count_picture_pixels(img, box, pred):
    """Pixels in `box` (fractions of the 4:3 picture centred in the window) satisfying pred(r,g,b)."""
    w, h, px = img
    pw = min(w, int(round(h * 4 / 3)))
    x_off = (w - pw) // 2
    X0, X1 = x_off + int(pw * box[0]), x_off + int(pw * box[1])
    Y0, Y1 = int(h * box[2]), int(h * box[3])
    n = 0
    for y in range(Y0, Y1):
        row = y * w * 3
        for x in range(X0, X1):
            i = row + x * 3
            if pred(px[i], px[i + 1], px[i + 2]):
                n += 1
    return n


def menu_font(r, g, b):
    return r > 170 and r > g > b and b < 110


def judge(code, meta, img, settings_text, frames):
    """Returns [(ok, what)]."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(code == 0, "runner exit 0 (the session ran to its cap; got %d)" % code)
    check(meta.get("finalized") == "1", "run.meta finalized=1 (got %s)" % meta.get("finalized"))
    check(meta.get("frames") == str(frames), "run.meta frames=%d (got %s)" % (frames, meta.get("frames")))
    check(meta.get("park") == "none", "run.meta park=none (got %s)" % meta.get("park", "<absent>"))
    check(meta.get("video") == "gl", "run.meta video=gl (got %s)" % meta.get("video"))
    me = census(meta.get("menu_ext"))
    check(me.get("state") == "registered" and me.get("pages") == "2" and me.get("items") == str(ITEMS) and me.get("texts") == str(TEXTS),
          "menu_ext state=registered pages=2 items=%d texts=%d (got %s)" % (ITEMS, TEXTS, meta.get("menu_ext", "<absent>")))
    check(me.get("changes") == "2", "menu_ext changes=2 (the scale step and the vsync toggle applied from the title's menu; got %s)" % me.get("changes", "<absent>"))
    pages = meta.get("menu_ext_pages", "")
    # Parked: the render-rate row is hidden from the Display page (the setting stays).
    check("video.render_rate" not in pages,
          "menu_ext_pages carries no video.render_rate row (the row is parked and hidden; got %s)" % (pages or "<absent>"))
    check(("video.scale=%d" % SCALE_AFTER) in pages and "video.vsync=1" in pages,
          "menu_ext_pages video.scale=%d and video.vsync=1 (the items hold the new values; got %s)" % (SCALE_AFTER, pages or "<absent>"))
    rows = census(meta.get("display_rows"))
    check(rows.get("scale_setting") == str(SCALE_AFTER) and rows.get("scale") == str(SCALE_AFTER),
          "display_rows scale_setting=%d scale=%d (the ScalePolicy factor every owned pass reads; got %s)" % (SCALE_AFTER, SCALE_AFTER, meta.get("display_rows", "<absent>")))
    check(rows.get("scale_requests") == "1" and rows.get("scale_applied") == "1",
          "display_rows scale_requests=1 scale_applied=1 (ONE apply on the backend owner; got requests=%s applied=%s)" % (rows.get("scale_requests"), rows.get("scale_applied")))
    check(rows.get("scale_async") == "1" and rows.get("scale_inline") == "0",
          "display_rows scale_async=1 scale_inline=0 (the ordered job on the GS consumer, no wait on the game thread; got async=%s inline=%s)" % (rows.get("scale_async"), rows.get("scale_inline")))
    check(rows.get("grab") == GRAB_AFTER,
          "display_rows grab=%s (the consumer's latest display grab -- the presented frame -- at scale %d; got %s)" % (GRAB_AFTER, SCALE_AFTER, rows.get("grab", "<absent>")))
    gc = census(meta.get("gfx_consumer"))
    try:
        resizes = int(gc.get("resizes", "0"))
    except ValueError:
        resizes = 0
    check(resizes >= 1, "gfx_consumer resizes >= 1 (the targets re-created at the new size; got %s)" % gc.get("resizes", "<absent>"))
    check(rows.get("vsync") == "1", "display_rows vsync=1 (the swap interval accepted; got %s)" % rows.get("vsync", "<absent>"))
    if img is None:
        check(False, "w_final.ppm present and readable (the presented window image)")
    else:
        menu_px = count_picture_pixels(img, MENU_BOX, menu_font)
        check(menu_px >= MENU_MIN, "the Display page is still drawn with the title's menu font after both changes (%d >= %d orange pixels in the items box)" % (menu_px, MENU_MIN))
    text = settings_text or ""
    check(re.search(r"^\s*video\.scale\s*=\s*%d\s*$" % SCALE_AFTER, text, re.M) is not None,
          "<user data>/ps2r.ini carries `video.scale = %d` (persisted; file: %r)" % (SCALE_AFTER, text[:120]))
    check(re.search(r"^\s*video\.vsync\s*=\s*on\s*$", text, re.M) is not None,
          "<user data>/ps2r.ini carries `video.vsync = on` (persisted; file: %r)" % text[:120])
    return results


def fake_image(menu=True, w=960, h=672):
    px = bytearray(w * h * 3)
    if menu:
        pw = int(h * 4 / 3)
        x_off = (w - pw) // 2
        for y in range(int(h * 0.35), int(h * 0.45)):
            for x in range(x_off + int(pw * 0.35), x_off + int(pw * 0.6)):
                i = (y * w + x) * 3
                px[i:i + 3] = bytes((230, 150, 40))
    return w, h, bytes(px)


GOOD_ROWS = ("scale_setting=2,scale=2,scale_requests=1,scale_applied=1,scale_async=1,scale_inline=0,grab=1024x896,"
             "vsync=1,fullscreen=0,aspect=0,render_rate=60,present_hz=60,presents=6500")
GOOD_FILE = "video.scale = 2\nvideo.vsync = on\n"


def selftest():
    ok = True
    reg = "state=registered,constructs=1,pages=2,items=%d,texts=%d,banks=1,text_base=228,boundaries=3590,changes=%%s,pushes=0,revalidations=0" % (ITEMS, TEXTS)
    good = {"finalized": "1", "frames": "3700", "park": "none", "video": "gl", "menu_ext": reg % "2",
            "menu_ext_pages": "page0=0xbe1e70,ui.perf_overlay=0,video.fullscreen=0,video.scale=2,video.vsync=1,video.aspect=0;page1=0xbe20a0,input.kbm_pad=0",
            "display_rows": GOOD_ROWS, "gfx_consumer": "draws=1,jobs=1,frames=3700,uploads=1,uploads_dropped=0,resizes=3,rebinds=0,warmed=1"}
    cases = [
        ("green", 0, good, fake_image(), GOOD_FILE, 0),
        ("red: the request taken but never applied (no DisplayRows job): the grab still 512x448, no resize",
         0, dict(good, display_rows=GOOD_ROWS.replace("scale=2,scale_requests=1,scale_applied=1,scale_async=1", "scale=1,scale_requests=0,scale_applied=0,scale_async=0").replace("grab=1024x896", "grab=512x448"),
                 gfx_consumer="draws=1,jobs=1,frames=3700,uploads=1,uploads_dropped=0,resizes=0,rebinds=0,warmed=1"),
         fake_image(), GOOD_FILE, 5),
        ("red: the apply waited on the game thread (inline)", 0, dict(good, display_rows=GOOD_ROWS.replace("scale_async=1,scale_inline=0", "scale_async=0,scale_inline=1")), fake_image(), GOOD_FILE, 1),
        ("red: vsync refused", 0, dict(good, display_rows=GOOD_ROWS.replace("vsync=1", "vsync=0")), fake_image(), "video.scale = 2\n", 2),
        ("red: a crash after the step", 1, dict(good, frames="3431", park="exception", finalized="0"), None, GOOD_FILE, 5),
        ("red: the scale persisted, vsync not", 0, good, fake_image(), "video.scale = 2\n", 1),
        ("red: the menu gone from the presented frame", 0, good, fake_image(menu=False), GOOD_FILE, 1),
        ("red (parked): the render-rate row back on the Display page (items=9, the key present)", 0,
         dict(good, menu_ext=reg.replace("items=%d" % ITEMS, "items=9") % "2",
              menu_ext_pages=good["menu_ext_pages"].replace("video.aspect=0;", "video.aspect=0,video.render_rate=0;")), fake_image(), GOOD_FILE, 2),
        ("red: the items never changed (changes=0, values old)", 0,
         dict(good, menu_ext=reg % "0", menu_ext_pages=good["menu_ext_pages"].replace("video.scale=2,video.vsync=1", "video.scale=1,video.vsync=0")), fake_image(), GOOD_FILE, 2),
    ]
    for name, code, meta, img, text, want_red in cases:
        r = judge(code, meta, img, text, 3700)
        red = sum(1 for x, _ in r if not x)
        good_case = red == want_red
        ok &= good_case
        print("  %s %s (red=%d want %d)" % ("ok " if good_case else "BAD", name, red, want_red))
        if not good_case:
            for x, what in r:
                if not x:
                    print("      FAIL " + what)
    print("root_display_rows_live selftest: %s (%d cases)" % ("PASS" if ok else "FAIL", len(cases)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--inputs", help="the pad script (default: beside this file)")
    ap.add_argument("--frames", type=int, default=3700)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", help="copy run.meta, w_final.ppm, ps2r.ini and the leg log here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    inputs = args.inputs or os.path.join(os.path.dirname(os.path.abspath(__file__)), "root_display_rows_live.padlog")
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    root = tempfile.mkdtemp(prefix="root_display_rows_live_")
    out, dump, ud, mc = (os.path.join(root, d) for d in ("out", "dump", "user_data", "mc"))
    for d in (out, dump, ud, mc):
        os.makedirs(d)
    log = os.path.join(root, "leg.log")
    cmd = [args.binary, "--profile", "default", "--harness-window", "--harness-out", out,
           "--game-files", os.path.abspath(args.game_files), "--frames", str(args.frames), "--inputs", os.path.abspath(inputs),
           "--dump-frames", dump, "--set", "ui.gui_open=off",
           "--set", "video.vsync=off",   # the test toggles vsync ON from the title's menu (the default is on)
           "--set", "paths.user_data=" + ud, "--set", "paths.mc_dir=" + mc]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out, "frames.fhash")
    env["RB_STALL_S"] = str(min(args.timeout, 120))
    proc = subprocess.run([args.runner, str(args.timeout), log, "--"] + cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    meta = parse_meta(os.path.join(out, "run.meta"))
    for key in ("frames", "finalized", "park", "park_detail", "video", "menu_ext", "menu_ext_pages", "display_rows", "gfx_consumer", "video_scale"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    if proc.returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    settings_path = os.path.join(ud, "ps2r.ini")
    settings = open(settings_path).read() if os.path.isfile(settings_path) else ""
    print("settings %s: %r" % (settings_path, settings))
    img = read_ppm(os.path.join(dump, "w_final.ppm"))
    failures = 0
    for ok, what in judge(proc.returncode, meta, img, settings, args.frames):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        for src in (os.path.join(out, "run.meta"), os.path.join(dump, "w_final.ppm"), settings_path, log):
            if os.path.isfile(src):
                shutil.copy(src, os.path.join(args.keep, os.path.basename(src)))
        print("kept at %s" % args.keep)
    if failures:
        print("RootDisplayRowsLive: FAIL (%d) -- leg kept at %s" % (failures, root))
        return 1
    print("RootDisplayRowsLive: ALL PASS (the internal scale stepped to 2x and vsync toggled from the title's Display page took effect live and persisted)")
    shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
