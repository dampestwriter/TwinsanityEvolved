#!/usr/bin/env python3
"""RootLoaderRingCache -- the composition root's loader seam under `loader.ring_cache=on`
reproduces the console's Default.rm2 stream ring at coldboot boundary 120.

Runs the built `nativeee` the way the harness does (a deterministic headless coldboot
leg of 121 frames, `--state-every 120 --set loader.ring_cache=on` -- the key is passed
explicitly although `on` has been the Config default,
so the pin names the rule it holds whatever the default is) and asserts on the
state_b120 image and run.meta:

  * the ring 0x948540-0x968540 holds the console's bytes (pcsx2_rec159_ee.bin, the
    PCSX2 state at the same record count 0x323278 = 0x159): the EOF fill
    [0x948540, 0x95e527) over the previous window's tail [0x95e52e, 0x968540) -- the
    7 bytes between are a title write past the fill, not the ring's;
  * the loader object's words equal the console's: fs+0 = 0x20300 (no window valid,
    no read pending after the stream reset), fs+0xc = 0, fs+0x10 = 0,
    fs+0x18 = 0x14628, the stream cursor at base;
  * run.meta loader_ring=on,hits=887,straddles=12,misses=12,fills=24,direct=1 -- the
    900 Default.rm2 records of boundary 112 served by 887 hits, 12 straddles and
    7 misses (19 fills), the other five fills the object's other files / objects;
  * finalized=1, frames=121.

A real process run, not a self-report. Exit 77 (SKIP) when the title files or the
console image are absent.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "verify"))
import memory_capture  # noqa: E402

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
CONSOLE_IMAGE = "pcsx2_rec159_ee.bin"
RING_LO, RING_HI = 0x948540, 0x968540
EOF_FILL_END, PREV_TAIL_START = 0x95e527, 0x95e52e
FS, CB = 0xbb3560, 0xbab588
EXPECT_RING = "on,hits=887,straddles=12,misses=12,fills=24,direct=1"


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    return meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--console-dir", required=True, help="directory holding " + CONSOLE_IMAGE)
    ap.add_argument("--frames", type=int, default=121)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection forwarded to the binary (e.g. hle.graphics_frame=off: the retained arm this pin was recorded under)")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    console = os.path.join(args.console_dir, CONSOLE_IMAGE)
    if not os.path.isfile(console):
        print("SKIP: console image absent: %s" % console)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_loader_ring_cache_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames), "--state-every", str(args.frames - 1),
           "--set", "loader.ring_cache=on",   # explicit: the pin is the ring rule, not the default
           "--game-files", os.path.abspath(args.game_files)]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    rc = subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fails = []
    try:
        meta_path = os.path.join(out_dir, "run.meta")
        if rc != 0 or not os.path.isfile(meta_path):
            print("FAIL: leg rc=%d, run.meta %s" % (rc, "present" if os.path.isfile(meta_path) else "absent"))
            return 1
        meta = parse_meta(meta_path)
        for k, want in (("finalized", "1"), ("frames", str(args.frames)), ("loader_ring", EXPECT_RING)):
            got = meta.get(k)
            print("%s %s=%s (want %s)" % ("PASS" if got == want else "FAIL", k, got, want))
            if got != want:
                fails.append(k)
        ram = memory_capture.read(out_dir, args.frames - 1)
        with open(console, "rb") as f:
            con = f.read()

        def words_differing(lo, hi):
            return sum(1 for a in range(lo, hi, 4) if ram[a:a + 4] != con[a:a + 4])

        d1 = words_differing(RING_LO, EOF_FILL_END)
        d2 = words_differing(PREV_TAIL_START, RING_HI)
        print("%s ring EOF fill 0x%x-0x%x: %d words differ from the console" % ("PASS" if d1 == 0 else "FAIL", RING_LO, EOF_FILL_END, d1))
        print("%s ring previous tail 0x%x-0x%x: %d words differ from the console" % ("PASS" if d2 == 0 else "FAIL", PREV_TAIL_START, RING_HI, d2))
        if d1 or d2:
            fails.append("ring")

        def w(buf, a):
            return struct.unpack_from("<I", buf, a)[0]

        for name, addr in (("fs+0", FS), ("fs+0xc", FS + 0xc), ("fs+0x10", FS + 0x10), ("fs+0x18", FS + 0x18), ("cb.cursor", CB + 0xc)):
            ok = w(ram, addr) == w(con, addr)
            print("%s %s = 0x%x (console 0x%x)" % ("PASS" if ok else "FAIL", name, w(ram, addr), w(con, addr)))
            if not ok:
                fails.append(name)
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
    print("FAILED: " + ",".join(fails) if fails else "ALL PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
