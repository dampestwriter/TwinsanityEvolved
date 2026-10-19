#!/usr/bin/env python3
"""RootMenuPerfOverlay -- the first option of the title's ADDED options pages, end to end on the
actual play binary: the owned menu extension (MenuExtension)
registers a 'Display' page with the title's Options page at the title's own OptionsPage::Construct
(NTSC 0x1646c0), the title's menu code navigates it and draws it with the title's menu font, the
'fps overlay' ToggleItem flips ui.perf_overlay, the presenter shows the game Hz / render fps
overlay in the PRESENTED frame, and the value persists in the settings file of the user data
root (never the memory card).

Two play legs (`--harness-window`, the default profile, `--inputs` = the pad script
tests/app/root_menu_perf_overlay.padlog: title -> START -> main menu -> options -> the added
'display' link -> the Display page), each with `--dump-frames <dir>` so the session's LAST window
image (the picture plus the GUI overlays: what the user sees) is written as w_final.ppm, and
`--set paths.user_data=<tmp>` so the settings file lands in the leg's own root:

  leg A (--frames 3300): stops ON the Display page, the toggle untouched.
  leg B (--frames 3600): the script presses CROSS on 'fps overlay' at frame 3330.

Judged (every line read from run.meta / the settings file / the window image):
  * both legs: runner exit 0, run.meta finalized=1 frames=N park=none video=gl,
    menu_ext state=registered,pages=2,items=ITEMS,texts=TEXTS,banks=1 (the extension registered at
    the title's construction: both pages, every declared widget (ITEMS; the Display page's A2
    texture filter / anisotropic rows included), every added string (TEXTS = MenuStrings::kCount)
    in the one bank the US disc carries), menu_ext_pages names ui.perf_overlay.
  * the PAGE RENDERS with the title's font: w_final has >= MENU_MIN title-font pixels (the menu
    font's orange fill: R > 170, R > G > B, B < 110) in the items box of the 4:3 picture, both legs.
  * leg A: menu_ext changes=0, ui.perf_overlay=0 in menu_ext_pages, NO overlay (< OVERLAY_OFF_MAX
    white pixels in the top-right box), no settings file write of the key.
  * leg B: menu_ext changes=1 (the user's toggle, applied from the title's menu), ui.perf_overlay=1,
    the overlay VISIBLE (>= OVERLAY_MIN white pixels in the top-right box: the overlay's text),
    <root>/ps2r.ini carries `ui.perf_overlay = on`.

--selftest judges fake inputs: a registered-but-untoggled leg B, a missing overlay with the
census green, a refused extension, a crash, a settings file without the key are red cases.
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
MENU_BOX = (0.30, 0.68, 0.14, 0.60)   # x0,x1,y0,y1 as fractions of the PICTURE (4:3, letterboxed in the window): the
                                      # items ellipse; the HIGHLIGHTED row (bright orange) is the first item, which the
                                      # eight-row Display page (rows + the A2 filter rows) draws at y ~0.17-0.24
                                      # (the six-row page drew it at ~0.26; the page is centred in the ellipse)
OVERLAY_BOX = (0.80, 1.00, 0.00, 0.13)   # fractions of the WINDOW (top-right)
MENU_MIN = 600
ITEMS = 8     # Display 7 (fps overlay, display mode, scale, vsync, aspect, texture filter, anisotropic;
              # video.render_rate PARKED/hidden
TEXTS = 35    # MenuStrings::kCount (24 rows + 11 texture filter / anisotropy strings)
OVERLAY_MIN = 120
OVERLAY_OFF_MAX = 20


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


def count_pixels(img, box, pred, picture=False):
    """Pixels in `box` (fractions) satisfying pred(r,g,b). picture=True: the box is of the 4:3
    picture centred in the window (the presenter letterboxes); False: of the whole window."""
    w, h, px = img
    if picture:
        pw = min(w, int(round(h * 4 / 3)))
        x_off = (w - pw) // 2
        X0, X1 = x_off + int(pw * box[0]), x_off + int(pw * box[1])
    else:
        X0, X1 = int(w * box[0]), int(w * box[1])
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


def white(r, g, b):
    return r > 200 and g > 200 and b > 200


def judge(leg, meta, img, settings_text, frames):
    """leg 'A' | 'B'. Returns [(ok, what)]."""
    results = []

    def check(ok, what):
        results.append((bool(ok), what))

    check(meta.get("finalized") == "1", "%s run.meta finalized=1 (got %s)" % (leg, meta.get("finalized")))
    check(meta.get("frames") == str(frames), "%s run.meta frames=%d (got %s)" % (leg, frames, meta.get("frames")))
    check(meta.get("park") == "none", "%s run.meta park=none (got %s)" % (leg, meta.get("park", "<absent>")))
    check(meta.get("video") == "gl", "%s run.meta video=gl (got %s)" % (leg, meta.get("video")))
    me = census(meta.get("menu_ext"))
    check(me.get("state") == "registered", "%s menu_ext state=registered (got %s)" % (leg, me.get("state", "<absent>")))
    # items: Display = fps overlay, display mode, internal scale, vsync, aspect ratio, render rate,
    # texture filter, anisotropic (A2); Controls = keyboard and mouse. texts: every MenuStrings id.
    check(me.get("pages") == "2" and me.get("items") == str(ITEMS) and me.get("texts") == str(TEXTS) and me.get("banks") == "1",
          "%s menu_ext pages=2,items=%d,texts=%d,banks=1 (the title's construction built both pages; got %s)" % (leg, ITEMS, TEXTS, meta.get("menu_ext", "<absent>")))
    pages = meta.get("menu_ext_pages", "")
    # Parked: the render-rate row is hidden from the Display page (the setting stays).
    check("video.render_rate" not in pages,
          "%s menu_ext_pages carries no video.render_rate row (the row is parked and hidden; got %s)" % (leg, pages or "<absent>"))
    want_value = "1" if leg == "B" else "0"
    check(("ui.perf_overlay=" + want_value) in pages,
          "%s menu_ext_pages ui.perf_overlay=%s (got %s)" % (leg, want_value, pages or "<absent>"))
    want_changes = "1" if leg == "B" else "0"
    check(me.get("changes") == want_changes,
          "%s menu_ext changes=%s (%s; got %s)" % (leg, want_changes,
                                                  "the user's toggle applied from the title's menu" if leg == "B" else "nothing toggled",
                                                  me.get("changes", "<absent>")))
    if img is None:
        check(False, "%s w_final.ppm present and readable (the presented window image)" % leg)
    else:
        menu_px = count_pixels(img, MENU_BOX, menu_font, picture=True)
        check(menu_px >= MENU_MIN, "%s the Display page is drawn with the title's menu font (%d >= %d orange pixels in the items box)" % (leg, menu_px, MENU_MIN))
        overlay_px = count_pixels(img, OVERLAY_BOX, white)
        if leg == "B":
            check(overlay_px >= OVERLAY_MIN, "B the perf overlay is VISIBLE in the presented frame (%d >= %d white pixels top-right)" % (overlay_px, OVERLAY_MIN))
        else:
            check(overlay_px < OVERLAY_OFF_MAX, "A no perf overlay in the presented frame (%d < %d white pixels top-right)" % (overlay_px, OVERLAY_OFF_MAX))
    has_key = re.search(r"^\s*ui\.perf_overlay\s*=\s*on\s*$", settings_text or "", re.M) is not None
    if leg == "B":
        check(has_key, "B <user data>/ps2r.ini carries `ui.perf_overlay = on` (the setting persisted; file: %r)" % (settings_text or "")[:120])
    else:
        check(not has_key, "A the settings file does not carry ui.perf_overlay=on (nothing toggled)")
    return results


def fake_image(menu=True, overlay=False, w=960, h=672):
    px = bytearray(w * h * 3)
    if menu:
        pw = int(h * 4 / 3)
        x_off = (w - pw) // 2
        for y in range(int(h * 0.35), int(h * 0.45)):
            for x in range(x_off + int(pw * 0.35), x_off + int(pw * 0.6)):
                i = (y * w + x) * 3
                px[i:i + 3] = bytes((230, 150, 40))
    if overlay:
        for y in range(10, 40):
            for x in range(w - 120, w - 20):
                i = (y * w + x) * 3
                px[i:i + 3] = bytes((255, 255, 255))
    return w, h, bytes(px)


def selftest():
    ok = True
    reg = "state=registered,constructs=1,pages=2,items=%d,texts=%d,banks=1,text_base=228,boundaries=3192,changes=%%s,pushes=0,revalidations=0" % (ITEMS, TEXTS)
    meta_a = {"finalized": "1", "frames": "3300", "park": "none", "video": "gl", "menu_ext": reg % "0",
              "menu_ext_pages": "page0=0xbe1e70,ui.perf_overlay=0,video.fullscreen=0,video.scale=1,video.vsync=0,video.aspect=0;page1=0xbe1f60,input.kbm_pad=0"}
    meta_b = dict(meta_a, frames="3600", menu_ext=reg % "1",
                  menu_ext_pages="page0=0xbe1e70,ui.perf_overlay=1,video.fullscreen=0,video.scale=1,video.vsync=0,video.aspect=0;page1=0xbe1f60,input.kbm_pad=0")
    r = judge("A", meta_a, fake_image(True, False), "", 3300)
    ok &= all(x for x, _ in r)
    r = judge("B", meta_b, fake_image(True, True), "ui.perf_overlay = on\n", 3600)
    ok &= all(x for x, _ in r)
    # red: leg B where the toggle never applied (census says 0 changes, value 0, no overlay, no file)
    r = judge("B", dict(meta_b, menu_ext=reg % "0", menu_ext_pages=meta_a["menu_ext_pages"]), fake_image(True, False), "", 3600)
    ok &= sum(1 for x, _ in r if not x) == 4
    # red: census green but the presented frame shows no overlay
    r = judge("B", meta_b, fake_image(True, False), "ui.perf_overlay = on\n", 3600)
    ok &= sum(1 for x, _ in r if not x) == 1
    # red: the extension refused (the title's page not recognised): pages untouched, no font text
    r = judge("A", dict(meta_a, menu_ext="state=refused,constructs=1,pages=0,items=0,texts=0,banks=0,text_base=0,boundaries=0,changes=0,pushes=0,revalidations=0,refusal=options page vtable mismatch", menu_ext_pages=""),
              fake_image(False, False), "", 3300)
    ok &= sum(1 for x, _ in r if not x) == 4
    # red: the render-rate row back on the Display page (items=9, the key present)
    r = judge("A", dict(meta_a, menu_ext=reg.replace("items=%d" % ITEMS, "items=9") % "0",
                        menu_ext_pages=meta_a["menu_ext_pages"].replace("video.aspect=0;", "video.aspect=0,video.render_rate=0;")),
              fake_image(True, False), "", 3300)
    ok &= sum(1 for x, _ in r if not x) == 2
    # red: a crash after the toggle
    r = judge("B", dict(meta_b, frames="3331", park="exception"), None, "ui.perf_overlay = on\n", 3600)
    ok &= sum(1 for x, _ in r if not x) == 3
    # red: the value flipped in the menu but was never persisted
    r = judge("B", meta_b, fake_image(True, True), "video.scale = 1\n", 3600)
    ok &= sum(1 for x, _ in r if not x) == 1
    print("root_menu_perf_overlay selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run(args, cmd, log, progress):
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = progress
    env["RB_STALL_S"] = str(min(args.timeout, 120))
    proc = subprocess.run([args.runner, str(args.timeout), log, "--"] + cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--inputs", help="the pad script (default: beside this file)")
    ap.add_argument("--frames-a", type=int, default=3300)
    ap.add_argument("--frames-b", type=int, default=3600)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    inputs = args.inputs or os.path.join(os.path.dirname(os.path.abspath(__file__)), "root_menu_perf_overlay.padlog")
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    root = tempfile.mkdtemp(prefix="root_menu_perf_overlay_")
    gf = os.path.abspath(args.game_files)
    failures = 0
    for leg, frames in (("A", args.frames_a), ("B", args.frames_b)):
        out = os.path.join(root, leg, "out")
        dump = os.path.join(root, leg, "dump")
        ud = os.path.join(root, leg, "user_data")
        mc = os.path.join(root, leg, "mc")
        for d in (out, dump, ud, mc):
            os.makedirs(d)
        log = os.path.join(root, leg, "leg.log")
        code = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", out,
                          "--game-files", gf, "--frames", str(frames), "--inputs", os.path.abspath(inputs),
                          "--dump-frames", dump, "--set", "ui.gui_open=off",
                          "--set", "paths.user_data=" + ud, "--set", "paths.mc_dir=" + mc], log, os.path.join(out, "frames.fhash"))
        meta = parse_meta(os.path.join(out, "run.meta"))
        for key in ("frames", "finalized", "park", "park_detail", "video", "menu_ext", "menu_ext_pages"):
            print("%s run.meta %s=%s" % (leg, key, meta.get(key, "<absent>")))
        if code == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
            print("SKIP: no window (video=stub)")
            shutil.rmtree(root, ignore_errors=True)
            return 77
        settings_path = os.path.join(ud, "ps2r.ini")
        settings = open(settings_path).read() if os.path.isfile(settings_path) else ""
        print("%s settings %s: %r" % (leg, settings_path, settings))
        img = read_ppm(os.path.join(dump, "w_final.ppm"))
        for ok, what in judge(leg, meta, img, settings, frames):
            print("  %s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
        if code != 0:
            print("  FAIL: %s runner exit %d (the session must run to its cap)" % (leg, code))
            failures += 1
    if failures:
        print("RootMenuPerfOverlay: FAIL (%d) -- legs kept at %s" % (failures, root))
        return 1
    print("RootMenuPerfOverlay: PASS (the title's added Display page toggled ui.perf_overlay; the overlay is in the presented frame; the setting persisted)")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print("kept at %s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
