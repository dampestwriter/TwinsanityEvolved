#!/usr/bin/env python3
"""RootTypeRegistryComplete -- a deterministic coldboot leg builds the title's type registry as
the console does: 28 registered entries, 126 live objects of the registry's 92-byte class.

The title's UI constructor (FUN_001700d8 -> FUN_00164ae8) registers four type descriptors per
memory-card slot it manages: the loop bound is the slot-count nibble of the MC manager's header
word (`*(*(gp-0x7dcc) + 0x14) & 0xf`, built as 4 by FUN_002a2700 for 2 ports x 2 slots) and each
iteration's registry object owns a 4-entry table; the seven tables are allocated back to back at
0xba8a60 (28 pointer slots). On the console (pcsx2_rec159_ee.bin, PCSX2 at record count 0x159)
every slot is filled and the page pool holds 126 USED chunks of class 22 (92 B). With the inherited
stack read of sceMcGetInfo's fifth argument (*format) -- `hle.libmc_format_in_t0=off`, the red
arm -- the row stores `format = 1` into that header word, collapsing the nibble to 1: one
iteration, 8 slots, 114 class-22 objects. The pin runs the `on` arm (t0, the EE convention).

Runs the built `nativeee` as the harness does (`--frames 121 --state-every 120
--set hle.libmc_format_in_t0=<arm>`, deterministic, headless) and walks state_b120 with the title's own layouts (verify/slab_walk.Pool: page-pool
header 0x31aff8, 63 size classes) against the saved console image:
  * the slot-count nibble of the MC header word equals the console's (4);
  * the registry table's non-null slot count equals the console's (28) and every slot's object
    carries the console's class signature (first word + the byte at +0x10; the pointers
    themselves differ by relocation and are not compared);
  * the class-22 USED chunk count equals the console's (126);
  * run.meta finalized=1, frames=121, libmc_format_in_t0=<arm>.
A real process run, not a self-report. Exit 77 (SKIP) when the title files or the console image
are absent.
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
import console_compare  # noqa: E402
import memory_capture  # noqa: E402
import slab_walk  # noqa: E402

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
CONSOLE_IMAGE = "pcsx2_rec159_ee.bin"
# title data (SLUS-20909): the page pool, the registry table, the MC manager's owner global
SLABPOOL, NCLS = 0x31aff8, 63
REGISTRY_TABLE, REGISTRY_SLOTS = 0xba8a60, 28
GP, GP_OWNER_OFF, OWNER_MC_OFF = 0x3114f0, -0x7dcc, 0x14
REGISTRY_CLASS = 22
RAM_LIMIT = 0x2000000


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    return meta


def rd32(m, a):
    return struct.unpack_from("<I", m.ram, a)[0]


def walk(path):
    """(slot-count nibble, [per-slot signature or None], class-22 used chunks)."""
    m = console_compare.Mem(path)
    nibble = None
    owner = rd32(m, GP + GP_OWNER_OFF)
    if 0 < owner < RAM_LIMIT:
        mc = rd32(m, owner + OWNER_MC_OFF)
        if 0 < mc < RAM_LIMIT:
            nibble = rd32(m, mc) & 0xf
    sigs = []
    for i in range(REGISTRY_SLOTS):
        p = rd32(m, REGISTRY_TABLE + 4 * i)
        if 0 < p < RAM_LIMIT - 0x14:
            sigs.append((rd32(m, p), m.ram[p + 0x10]))
        else:
            sigs.append(None)
    pool = slab_walk.Pool(m, SLABPOOL, NCLS)
    n22 = sum(1 for c, lo, sz in pool.used_chunks() if c == REGISTRY_CLASS)
    return nibble, sigs, n22


def fmt_sigs(sigs):
    return " ".join("-" if s is None else "%x/%02x" % s for s in sigs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--console-dir", required=True, help="directory holding " + CONSOLE_IMAGE)
    ap.add_argument("--frames", type=int, default=121)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--arm", choices=("on", "off"), default="on", help="the hle.libmc_format_in_t0 value under test (off = the red arm)")
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

    con_nibble, con_sigs, con_n22 = walk(console)
    con_filled = sum(1 for s in con_sigs if s is not None)
    print("console: slot-count nibble %s; registry slots %d/%d [%s]; class-%d used chunks %d"
          % (con_nibble, con_filled, REGISTRY_SLOTS, fmt_sigs(con_sigs), REGISTRY_CLASS, con_n22))

    out_dir = tempfile.mkdtemp(prefix="root_type_registry_%s_" % args.arm)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames), "--state-every", str(args.frames - 1),
           "--set", "hle.libmc_format_in_t0=%s" % args.arm,
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
        for k, want in (("finalized", "1"), ("frames", str(args.frames)), ("libmc_format_in_t0", args.arm)):
            got = meta.get(k)
            print("%s %s=%s (want %s)" % ("PASS" if got == want else "FAIL", k, got, want))
            if got != want:
                fails.append(k)
        state = memory_capture.path(out_dir, args.frames - 1)
        new_nibble, new_sigs, new_n22 = walk(state)
        new_filled = sum(1 for s in new_sigs if s is not None)
        print("NEW: slot-count nibble %s; registry slots %d/%d [%s]; class-%d used chunks %d"
              % (new_nibble, new_filled, REGISTRY_SLOTS, fmt_sigs(new_sigs), REGISTRY_CLASS, new_n22))
        ok = new_nibble is not None and new_nibble == con_nibble
        print("%s MC slot-count nibble NEW %s == console %s" % ("PASS" if ok else "FAIL", new_nibble, con_nibble))
        if not ok:
            fails.append("slot_nibble")
        ok = new_filled == con_filled == REGISTRY_SLOTS
        print("%s registry slots filled NEW %d == console %d == %d" % ("PASS" if ok else "FAIL", new_filled, con_filled, REGISTRY_SLOTS))
        if not ok:
            fails.append("registry_slots")
        bad = [i for i in range(REGISTRY_SLOTS) if new_sigs[i] != con_sigs[i]]
        ok = not bad
        print("%s every slot's object class signature equals the console's (differing slots: %s)" % ("PASS" if ok else "FAIL", bad))
        if not ok:
            fails.append("registry_classes")
        ok = new_n22 == con_n22
        print("%s class-%d used chunks NEW %d == console %d" % ("PASS" if ok else "FAIL", REGISTRY_CLASS, new_n22, con_n22))
        if not ok:
            fails.append("class22_census")
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
    print("FAILED: " + ",".join(fails) if fails else "ALL PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
