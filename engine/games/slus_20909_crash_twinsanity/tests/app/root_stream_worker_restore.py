#!/usr/bin/env python3
"""RootRestoresStreamWorker -- after an anchor restore the STREAM.IRX streaming
worker thread exists in the live IOP kernel, parked on its semaphore, and the
stream advances.

Runs the built `nativeee` binary as the harness does for an anchor leg
(`--profile deterministic --headless --harness-out <dir> --frames N
--state-every 1 --load-state <anchor> --game-files <dir>`), bounded through
verify/run_bounded.sh, and asserts on run.meta and the boundary images:

  * iop_threads_restored         -- the LIVE kernel table right after the restore
                                    carries the worker (tid 2) as Wait/Sema and the
                                    RPC thread (tid 1) as Wait/Sleep; sema 0 holds
                                    the image's token count (0:0)
  * iop_threads                  -- at finalize tid 2 is still registered and not
                                    Dormant (the worker keeps running the stream)
  * RAM0[0x3b3008] (the title's STREAM recv buffer, header word 2 = the stream
    position) advances between boundary 0 and boundary N-2, as OLD's paired
    images do (artdet: 0x5f20 at b0/b1 -> 0x6050 at b2), OR libsd_trans > 0
  * iopm_restored=1, restore_mhash_ok=1, finalized=1, frames=N

A real process run, not a self-report: a re-park that leaves the image's started
threads un-created shows `2:` absent from iop_threads_restored (the worker was
never created in the loader process -- the title's stream-open RPC created it on
the saver), sema 0 accumulating an unconsumed token (0:1) and the position word
frozen. Exit 77 (SKIP) when the title files or the anchor are absent.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
STREAM_POSITION_WORD = 0x003B3008   # the title's STREAM recv buffer, header word 2 (stream position)
WORKER_TID = 2                      # the module's streaming worker (entry 0x30d4, prio 40) in every anchor
RPC_TID = 1                         # the module's RPC dispatch thread (entry 0x64f4, prio 30)


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


def thread_states(census):
    """'1:Wait/Sleep,2:Wait/Sema' -> {1: 'Wait/Sleep', 2: 'Wait/Sema'}"""
    out = {}
    for item in census.split(","):
        if ":" not in item:
            continue
        tid, st = item.split(":", 1)
        try:
            out[int(tid)] = st
        except ValueError:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--anchor", required=True, help="a frozen anchor .ps2st (OLD save_states, read-only)")
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--verify-dir", required=True, help="the repo's verify/ (state_hash.py)")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=60)
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

    out_dir = tempfile.mkdtemp(prefix="root_stream_worker_restore_")
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
    for key in ("iopm_restored", "iop_threads_restored", "iop_threads", "iop_semas", "libsd_trans",
                "cdvd_deliveries", "libsd_calls", "restore_mhash_ok", "finalized", "frames"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    check(proc.returncode == 0, "bounded run exits 0")
    check(meta.get("iopm_restored") == "1", "iopm_restored=1")
    check(meta.get("restore_mhash_ok") == "1", "restore_mhash_ok=1")
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(args.frames), "frames=%d" % args.frames)

    restored = meta.get("iop_threads_restored", "")
    threads_part, _, semas_part = restored.partition(";")
    st = thread_states(threads_part)
    check(st.get(WORKER_TID) == "Wait/Sema",
          "right after the restore the worker (tid %d) is parked on its semaphore (Wait/Sema), not absent/Dormant" % WORKER_TID)
    check(st.get(RPC_TID) == "Wait/Sleep", "right after the restore the RPC thread (tid %d) keeps its Wait/Sleep park" % RPC_TID)
    check("0:0" in semas_part.split(","), "sema 0 holds the image's token count right after the restore (0:0, no unconsumed token)")
    final = thread_states(meta.get("iop_threads", ""))
    check(WORKER_TID in final and not final[WORKER_TID].startswith("Dormant"),
          "at finalize the worker (tid %d) is registered and not Dormant" % WORKER_TID)

    # The stream position word in the boundary images.
    def position(n):
        p = os.path.join(out_dir, "state_b%d.ps2st" % n)
        if not os.path.isfile(p):
            return None
        _, _, s = state_hash.parse(p)
        ram = s[state_hash.TAG_RAM][2]
        return struct.unpack_from("<I", ram, STREAM_POSITION_WORD)[0]

    positions = [position(n) for n in range(args.frames)]
    print("RAM0[0x%08x] per boundary: %s" % (STREAM_POSITION_WORD,
                                              " ".join("?" if v is None else "0x%x" % v for v in positions)))
    trans = meta.get("libsd_trans", "0/0/0").split("/")[0]
    try:
        trans_bytes = int(trans)
    except ValueError:
        trans_bytes = 0
    present = [v for v in positions if v is not None]
    advancing = len(present) >= 2 and present[-1] != present[0]
    check(advancing or trans_bytes > 0,
          "the stream advances after the restore: position word moves across the boundaries or libsd_trans > 0 "
          "(advancing=%s libsd_trans=%d)" % (advancing, trans_bytes))

    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        try:
            os.remove(log)
        except OSError:
            pass
    else:
        print("artifacts kept: %s %s" % (out_dir, log))
    print("RootRestoresStreamWorker: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
