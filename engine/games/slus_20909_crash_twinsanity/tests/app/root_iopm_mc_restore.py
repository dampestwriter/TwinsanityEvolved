#!/usr/bin/env python3
"""RootRegistersIopmSection -- the composition root registers the IOPM
savestate section and derives libmc's Bound from the restored client record.

Runs the built `nativeee` binary exactly as the harness does for an anchor leg
(verify/run_leg.py: `--profile deterministic --headless --harness-out <dir>
--frames N --state-every 1 --load-state <anchor> --game-files <dir>`), bounded
through verify/run_bounded.sh, and asserts on the produced run.meta and on the
boundary-0 image the leg drops:

  * iopm_restored=1              -- IopmSection::MakeSpec registered; the anchor's
                                    IOPM section restored once (after_restore ran)
  * iop_ram_hash_restored        -- equals FNV-1a64 of the anchor's own IOPM RAM
                                    bytes (payload[4 : 4 + 2 MB]): the IOP RAM the
                                    EE sees right after the restore IS the anchor's
  * iop_unrouted=0               -- no IOP access fell off the bus after the restore
  * mc_bound=1, mc_bound_source=restored_client_record
                                 -- libmc's readiness derived from the restored
                                    sceSifClientData's bound-server word
  * state_b0: RAM0[0x3b3004] == 0x7fff0020
                                 -- the STREAM.IRX module answered the title's
                                    first streaming RPC from its restored state
                                    (the cold module's reply word 1 is a counter,
                                    1 at boundary 0); the reply header is the
                                    title's recv buffer at 0x3b3000
  * restore_mhash_ok=1, finalized=1, frames=N

A real process run, not a self-report: a root that drops the IOPM registration
leaves iopm_restored=0 and the cold reply word; one that skips DeriveBound
leaves mc_bound=0. Exit 77 (SKIP) when the title files or the anchor are absent.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
STREAM_REPLY_WORD1 = 0x003B3004          # the title's STREAM recv buffer, header word 1
STREAM_REPLY_WORD1_RESTORED = 0x7FFF0020  # the live module's value at every anchor (OLD and the anchors themselves)
IOP_RAM_SIZE = 2 * 1024 * 1024
FNV_OFFSET, FNV_PRIME = 0xCBF29CE484222325, 0x100000001B3


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--verify-dir", required=True, help="the repo's verify/ (state_hash.py)")
    ap.add_argument("--frames", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=50)
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.anchor):
        print("SKIP: anchor absent: %s" % args.anchor)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    sys.path.insert(0, os.path.abspath(args.verify_dir))
    import state_hash  # noqa: E402

    out_dir = tempfile.mkdtemp(prefix="root_iopm_mc_restore_")
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless",
           "--harness-out", out_dir, "--frames", str(args.frames), "--state-every", "1",
           "--load-state", os.path.abspath(args.anchor),
           "--game-files", os.path.abspath(args.game_files)]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)

    failures = 0
    meta_path = os.path.join(out_dir, "run.meta")
    if not os.path.isfile(meta_path):
        print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
        return 1
    meta = parse_meta(meta_path)
    for key in ("iopm_restored", "iop_ram_hash_restored", "iop_unrouted", "mc_bound", "mc_bound_source",
                "restore_mhash_ok", "finalized", "frames", "stream_rpc_cmds"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("iopm_restored") == "1", "iopm_restored=1 (IopmSection registered; the anchor's IOPM restored once)")
    check(meta.get("iop_unrouted") == "0", "iop_unrouted=0")
    check(meta.get("mc_bound") == "1", "mc_bound=1")
    check(meta.get("mc_bound_source") == "restored_client_record",
          "mc_bound_source=restored_client_record (Bound derived from the restored sceSifClientData)")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d (the --frames cap ended the leg)" % args.frames)

    # The anchor's own IOPM payload: u32 ram_size, then the 2 MB of IOP RAM.
    tag_iopm = state_hash.tag("IOPM")
    _, _, anchor_sections = state_hash.parse(args.anchor)
    if tag_iopm in anchor_sections:
        payload = anchor_sections[tag_iopm][2]
        ram_size = struct.unpack_from("<I", payload, 0)[0]
        anchor_hash = fnv1a64(payload[4:4 + ram_size]) if ram_size == IOP_RAM_SIZE else None
        print("anchor IOPM ram_size=%d fnv1a64=%s" % (ram_size, "0x%x" % anchor_hash if anchor_hash is not None else "?"))
        check(anchor_hash is not None and meta.get("iop_ram_hash_restored") == "0x%x" % anchor_hash,
              "iop_ram_hash_restored equals the anchor's IOPM RAM hash")
    else:
        check(False, "the anchor carries an IOPM section")

    b0 = os.path.join(out_dir, "state_b0.ps2st")
    if os.path.isfile(b0):
        _, _, s = state_hash.parse(b0)
        ram = s[state_hash.TAG_RAM][2]
        word1 = struct.unpack_from("<I", ram, STREAM_REPLY_WORD1)[0]
        print("state_b0 RAM0[0x%08x]=0x%08x" % (STREAM_REPLY_WORD1, word1))
        check(word1 == STREAM_REPLY_WORD1_RESTORED,
              "STREAM reply header word 1 at boundary 0 is the restored module's 0x7fff0020 (not the cold counter)")
    else:
        check(False, "state_b0.ps2st written (--state-every 1)")

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootRegistersIopmSection: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
