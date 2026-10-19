#!/usr/bin/env python3
"""RootMetaArenaOff -- the composition root under `hle.meta_arena=off` keeps the title's
handle-heap descriptors on the title's page pool, as the console does.

Runs the built `nativeee` the way the harness does (a deterministic headless coldboot
leg of 121 frames, `--state-every 120 --set hle.meta_arena=<on|off>`) and walks the
state_b120 image with the title's own layouts (verify/console_compare.walk_records,
verify/slab_walk.Pool: record-pool header 0x31b138, live counter 0x323278, page-pool
header 0x31aff8 with 63 size classes) against the saved console image
`pcsx2_rec159_ee.bin` (PCSX2 at the same record count, 0x323278 = 0x159):

  off (the pin):
    * every record descriptor lives in RAM, none in the META arena: `desc in RAM 385 /
      META 0` on both the console and NEW; every one of them is a USED chunk of slab
      class 7 (32 B) on both;
    * the page pool holds the same number of class-7 pages as the console (12), and the
      console's first class-7 pages are NEW's (the pages before the first divergent pop
      agree class for class; the first divergent pop is reported, not asserted -- it is
      another class's: the objects hardware builds and NEW never builds);
    * run.meta `meta_arena=off,hooks=1,allocs=0,frees=0,...` (no allocation routed; the
      retained free body never fired on a fresh boot), finalized=1, frames=121.
  on (the red arm, `--arm on`): the same walk reports `desc in RAM 0 / META 385`, the
    descriptors in no slab class, fewer class-7 pages than the console -- the pin's
    assertions FAIL there, which is what proves they measure the routing.

A real process run, not a self-report. Exit 77 (SKIP) when the title files or the console
image are absent.
"""
import argparse
import os
import shutil
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
# title data (classes.tsv: the page pool and the record pool of SLUS-20909)
SLABPOOL, NCLS, POOL, USED = 0x31aff8, 63, 0x31b138, 0x323278
DESC_CLASS, DESC_CHUNK = 7, 32


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    return meta


def walk(path):
    """(desc_in_ram, desc_in_meta, {chunk key: n}, sorted class-7 page indices, {page idx: class})."""
    m = console_compare.Mem(path)
    recs, hdr = console_compare.walk_records(m, POOL, USED)
    pool = slab_walk.Pool(m, SLABPOOL, NCLS)
    chunks = {}
    for r in recs:
        d = r[3]
        if m.in_meta(d):
            key = "META"
        else:
            st = pool.status(d)
            key = ("cls %d/%dB" % (st[1], st[3])) if st and st[0] == "USED" else "outside-slab"
        chunks[key] = chunks.get(key, 0) + 1
    pages = {idx: pool.page[idx][0] for idx in pool.page}
    cls7 = sorted(idx for idx, c in pages.items() if c == DESC_CLASS)
    return hdr["desc_in_ram"], hdr["desc_in_meta"], chunks, cls7, pages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--console-dir", required=True, help="directory holding " + CONSOLE_IMAGE)
    ap.add_argument("--frames", type=int, default=121)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--arm", choices=("off", "on"), default="off", help="the hle.meta_arena value under test (on = the red arm)")
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

    con_ram, con_meta, con_chunks, con_cls7, con_pages = walk(console)
    print("console: desc in RAM %d / META %d; chunks %s; class-%d pages %d %s" % (con_ram, con_meta, con_chunks, DESC_CLASS, len(con_cls7), con_cls7))

    out_dir = tempfile.mkdtemp(prefix="root_meta_arena_%s_" % args.arm)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames), "--state-every", str(args.frames - 1),
           "--set", "hle.meta_arena=%s" % args.arm,
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
        for k, want in (("finalized", "1"), ("frames", str(args.frames))):
            got = meta.get(k)
            print("%s %s=%s (want %s)" % ("PASS" if got == want else "FAIL", k, got, want))
            if got != want:
                fails.append(k)
        ma = meta.get("meta_arena", "")
        want_prefix = "off,hooks=1,allocs=0,frees=0,"
        ok = ma.startswith(want_prefix)
        print("%s meta_arena=%s (want prefix %s)" % ("PASS" if ok else "FAIL", ma, want_prefix))
        if not ok:
            fails.append("meta_arena")
        state = memory_capture.path(out_dir, args.frames - 1)
        new_ram, new_meta, new_chunks, new_cls7, new_pages = walk(state)
        print("NEW %s: desc in RAM %d / META %d; chunks %s; class-%d pages %d %s" % (args.arm, new_ram, new_meta, new_chunks, DESC_CLASS, len(new_cls7), new_cls7))
        ok = new_meta == 0 and new_ram == con_ram and con_meta == 0
        print("%s descriptors: NEW RAM %d / META %d == console RAM %d / META 0" % ("PASS" if ok else "FAIL", new_ram, new_meta, con_ram))
        if not ok:
            fails.append("desc_location")
        key = "cls %d/%dB" % (DESC_CLASS, DESC_CHUNK)
        ok = new_chunks == {key: new_ram} and con_chunks == {key: con_ram}
        print("%s every descriptor a USED %s chunk on both (NEW %s, console %s)" % ("PASS" if ok else "FAIL", key, new_chunks, con_chunks))
        if not ok:
            fails.append("desc_class")
        ok = len(new_cls7) == len(con_cls7)
        print("%s class-%d page count NEW %d == console %d" % ("PASS" if ok else "FAIL", DESC_CLASS, len(new_cls7), len(con_cls7)))
        if not ok:
            fails.append("cls7_pages")
        # the pages before the first divergent pop agree class for class (reported, the first divergence named)
        diffs = sorted(i for i in set(con_pages) | set(new_pages) if con_pages.get(i) != new_pages.get(i))
        first = diffs[0] if diffs else None
        agree7 = [i for i in con_cls7 if first is None or i < first]
        ok = agree7 and all(new_pages.get(i) == DESC_CLASS for i in agree7)
        print("%s the console's class-%d pages before the first divergent pop are NEW's: %s (first divergent pop: page %s, console cls %s / NEW cls %s)"
              % ("PASS" if ok else "FAIL", DESC_CLASS, agree7, first, con_pages.get(first), new_pages.get(first)))
        if not ok:
            fails.append("cls7_prefix")
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
    print("FAILED: " + ",".join(fails) if fails else "ALL PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
