#!/usr/bin/env python3
"""root_texture_pack_title.py -- RootTexturePackTitle: A4 texture packs reach the PRESENTED frame.

Two deterministic 1000-frame Release coldboots to the title screen on the all-on composition:

  1. WITHOUT a pack (video.texture_pack=none): the identity arm. The census row
     `graphics_texture_pack` reads pack=none / replaced=0 and the presented frame p<key>
     holds (almost) no pixels of the synthetic pack colour. This is the red-before: the same
     judge against a run without the pack fails the "reads the pack colour" check.
  2. WITH a synthetic pack: `<data dir>/textures/<pack>/<hash>.png` for the title screen's
     logo tiles (the three 128x256 pictures the title draws in its upper-left logo band) and
     its prompt pictures, each a solid pack colour at TWICE the authored resolution (the
     resolution-independent sampling pin: the surface is 256x512, the title's tuple still
     names 128x256). The census reads replaced == the pack's file count and the presented
     frame's LOGO BAND is >= LOGO_SHARE_MIN pack-coloured.

The hashes are TextureContentHash values (xxHash64 over the authored RGBA8 levels; the key
`--dump-textures` names and a pack uses), read from a `--dump-textures` run of this binary:
int_work/m_series/graphics/lagrange_a4/dump_title/textures/index.tsv. They are stable across
runs by construction (a pure function of the converted pixels).

Usage: --binary --game-files --runner [--frames N] [--timeout S] [--keep]; --selftest judges
fake frames / fake run.meta with no game process.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_shadow_level_entry as shadow  # noqa: E402  (the leg mechanics, the ppm reader, counters)

FRAMES = 1000
PIN_FRAME = 900          # the title screen with its logo (frames 600..1150 of the deterministic coldboot)
# The title screen's pictures (TextureContentHash, authored size): the logo tiles and the prompt.
LOGO_TEXTURES = (
    ("a3390c92d7e8ab5d", 128, 256),
    ("315441410fed0dd9", 128, 256),
    ("4cae76ba06bc0422", 128, 256),
    ("f02c632b2d2d9fbb", 128, 256),
    ("794ef183583b9894", 128, 256),
    ("f958c8eb192feea4", 128, 128),
)
PACK_NAME = "root_pin"
PACK_COLOUR = (255, 0, 255)   # magenta: absent from the retail title screen
# Top-down logical 512x448 box (x0, x1, y0, y1) of the logo band the pack recolours.
LOGO_BOX = (0, 192, 0, 168)
LOGO_SHARE_MIN = 0.20         # measured 0.35..0.39 on p700..p1100 of this binary with the pack
LOGO_SHARE_MAX_WITHOUT = 0.01 # measured 0.000 without


def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make_pack(data_dir):
    directory = os.path.join(data_dir, "textures", PACK_NAME)
    os.makedirs(directory, exist_ok=True)
    for hex_hash, w, h in LOGO_TEXTURES:
        write_png(os.path.join(directory, hex_hash + ".png"), w * 2, h * 2, PACK_COLOUR + (255,))
    return directory


def is_pack_colour(r, g, b):
    return r > 180 and b > 180 and g < 80


def pack_share(path, box):
    w, h, rows = shadow.read_ppm(path, flip=False)   # the presented image is top-down
    x0, x1, y0, y1 = shadow.scaled(box, w / 512.0, h / 448.0)
    n = hits = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            n += 1
            if is_pack_colour(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]):
                hits += 1
    return hits / float(max(1, n)), (w, h)


def judge(name, returncode, meta, frames, frame_path, with_pack):
    results = []

    def check(ok, what):
        results.append((bool(ok), "%s: %s" % (name, what)))
        return bool(ok)

    check(returncode == 0, "bounded run exits 0 (exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") == "none", "no park (park=%s)" % meta.get("park", "none"))
    textures = shadow.counters(meta.get("graphics_textures", ""))
    check(textures.get("loaded", 0) > 0, "graphics_textures loaded>0 (textures at load converted the title's records; %s)"
          % meta.get("graphics_textures", "<absent>")[:60])
    pack = shadow.counters(meta.get("graphics_texture_pack", ""))
    row = meta.get("graphics_texture_pack", "<absent>")
    if with_pack:
        check(pack.get("files", 0) == len(LOGO_TEXTURES) and pack.get("hits", 0) == len(LOGO_TEXTURES) and
              pack.get("replaced", 0) == len(LOGO_TEXTURES) and pack.get("decode_failures", 1) == 0,
              "graphics_texture_pack files=hits=replaced=%d decode_failures=0 (%s)" % (len(LOGO_TEXTURES), row))
    else:
        check(row.startswith("pack=none,") and pack.get("replaced", 1) == 0 and pack.get("hits", 1) == 0,
              "graphics_texture_pack pack=none replaced=0 (the identity arm; %s)" % row)
    if not os.path.isfile(frame_path):
        check(False, "presented frame p%d dumped (%s)" % (PIN_FRAME, frame_path))
        return results
    share, dims = pack_share(frame_path, LOGO_BOX)
    if with_pack:
        check(share >= LOGO_SHARE_MIN, "presented frame p%d logo band reads the pack colour: share %.3f >= %.2f (%dx%d)"
              % (PIN_FRAME, share, LOGO_SHARE_MIN, dims[0], dims[1]))
    else:
        check(share <= LOGO_SHARE_MAX_WITHOUT, "presented frame p%d logo band holds no pack colour: share %.3f <= %.2f (%dx%d)"
              % (PIN_FRAME, share, LOGO_SHARE_MAX_WITHOUT, dims[0], dims[1]))
    return results


META_KEYS = ("frames", "finalized", "park", "graphics_textures", "graphics_texture_pack", "graphics_texture_uploads")
GOOD_WITH = ("frames=1000\nfinalized=1\npark=none\n"
             "graphics_textures=loaded=319,destroyed=8,model_calls=0,refused=0,failed=0\n"
             "graphics_texture_pack=pack=/tmp/x/textures/root_pin,files=6,lookups=319,hits=6,misses=313,replaced=6,"
             "decoded=6,decode_failures=0,prewarmed=6,cached_bytes=2883584,uncached=0\n")
GOOD_WITHOUT = ("frames=1000\nfinalized=1\npark=none\n"
                "graphics_textures=loaded=319,destroyed=8,model_calls=0,refused=0,failed=0\n"
                "graphics_texture_pack=pack=none,files=0,lookups=0,hits=0,misses=0,replaced=0,decoded=0,"
                "decode_failures=0,prewarmed=0,cached_bytes=0,uncached=0\n")


def fake_frame(path, logo_colour):
    def pixel(x, y):
        return logo_colour if (x < 192 and y < 168) else (60, 90, 140)
    shadow.write_ppm(path, 512, 448, pixel, flip=False)


def selftest():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="root_texture_pack_selftest_")
    try:
        with_pack = os.path.join(tmp, "with.ppm")
        without = os.path.join(tmp, "without.ppm")
        fake_frame(with_pack, PACK_COLOUR)
        fake_frame(without, (230, 160, 40))   # the retail logo's orange
        meta_with = {k: v for k, v in (l.split("=", 1) for l in GOOD_WITH.strip().split("\n"))}
        meta_without = {k: v for k, v in (l.split("=", 1) for l in GOOD_WITHOUT.strip().split("\n"))}
        cases = [
            ("good with pack", 0, meta_with, with_pack, True, None),
            ("good without pack", 0, meta_without, without, False, None),
            # red-before: the pack arm judged against a run that drew the retail logo
            ("pack configured, retail logo presented", 0, meta_with, without, True, "logo band reads the pack colour"),
            # the pack never engaged (hashes unknown to this binary / pack not found)
            ("pack configured, replaced=0", 0, dict(meta_with, graphics_texture_pack=meta_with["graphics_texture_pack"]
                                                     .replace("hits=6", "hits=0").replace("replaced=6", "replaced=0")),
             with_pack, True, "files=hits=replaced=6"),
            ("a refused PNG", 0, dict(meta_with, graphics_texture_pack=meta_with["graphics_texture_pack"]
                                      .replace("decode_failures=0", "decode_failures=1")), with_pack, True, "decode_failures=0"),
            # packs off must not recolour anything
            ("pack off, pack colour presented", 0, meta_without, with_pack, False, "holds no pack colour"),
            ("pack off, a pack engaged", 0, dict(meta_without, graphics_texture_pack=meta_with["graphics_texture_pack"]),
             without, False, "pack=none replaced=0"),
            ("textures at load off", 0, dict(meta_with, graphics_textures="loaded=0,destroyed=0,model_calls=319,refused=0,failed=0"),
             with_pack, True, "graphics_textures loaded>0"),
            ("a crash", 134, dict(meta_with, finalized="0"), with_pack, True, "exits 0"),
            ("no presented frame", 0, meta_with, os.path.join(tmp, "missing.ppm"), True, "dumped"),
        ]
        for name, rc, meta, frame, with_flag, expect_fail in cases:
            results = judge("case", rc, meta, FRAMES, frame, with_flag)
            failed = [what for ok, what in results if not ok]
            if expect_fail is None:
                ok = not failed
            else:
                ok = any(expect_fail in what for what in failed)
            print("%s selftest: %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else " -> " + "; ".join(failed)))
            failures += 0 if ok else 1
        # the pack writer produces files the codec names
        pack_dir = make_pack(os.path.join(tmp, "data"))
        names = sorted(os.listdir(pack_dir))
        ok = names == sorted(h + ".png" for h, _, _ in LOGO_TEXTURES)
        print("%s selftest: pack directory holds the %d <hash>.png files" % ("ok  " if ok else "FAIL", len(LOGO_TEXTURES)))
        failures += 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootTexturePackTitleSelftest: %s" % ("PASS" if not failures else "FAIL (%d)" % failures))
    return 1 if failures else 0


def run_leg(args, name, settings, frames_dir, out_dir):
    log = out_dir + ".log"
    os.makedirs(frames_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--dump-frames", frames_dir]
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
    root = tempfile.mkdtemp(prefix="root_texture_pack_title_")
    data_dir = os.path.join(root, "data")
    make_pack(data_dir)
    failures = 0
    logs = []
    for name, with_pack in (("without", False), ("with", True)):
        out_dir = os.path.join(root, name)
        frames_dir = os.path.join(out_dir, "frames")
        settings = ["paths.user_data=" + data_dir, "video.texture_pack=" + (PACK_NAME if with_pack else "none")]
        rc, meta, log = run_leg(args, name, settings, frames_dir, out_dir)
        logs.append(log)
        for ok, what in judge(name, rc, meta, args.frames, os.path.join(frames_dir, "p%d.ppm" % PIN_FRAME), with_pack):
            print("%s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
    if failures:
        print("RootTexturePackTitle: FAIL (%d) -- logs %s, out %s" % (failures, ", ".join(logs), root))
        return 1
    print("RootTexturePackTitle: ALL PASS")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
