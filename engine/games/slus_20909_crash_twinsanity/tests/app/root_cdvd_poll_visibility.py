#!/usr/bin/env python3
"""RootCdvdPollVisibility -- cdvd.delivery_visibility=poll: host-served
loader completions become visible at the poll that asks once the modelled drive is
done; a blocking wait whose drive end lies beyond the frame's edge parks whole frames
(the EE blocked while vblanks pass) instead of crossing the edge inside the wait.

Runs the built `nativeee` twice as the harness does for a deterministic headless
coldboot leg (`--profile deterministic --headless --harness-out <dir> --frames N
--game-files <dir>`), once with `--set cdvd.delivery_visibility=poll` and once with
`--set cdvd.delivery_visibility=boundary` (the pre-decision model; the default is poll), both bounded through verify/run_bounded.sh, and asserts:

  * both legs finalize at N frames (run.meta finalized=1, frames=N, park=none);
  * the poll leg's load-wait lift yielded at least once because nothing was due
    inside the frame (run.meta loader_spin_lifts idle > 0: BUG-070's fix -- the
    IOP's in-frame work runs at the poll, the frame ends otherwise), and the
    boundary leg never took that branch (idle=0, iop_pumps=0);
  * run.meta cdvd_visibility names the mode of each leg; the poll leg reports
    cdvd_poll_deliveries > 0, the boundary leg cdvd_poll_deliveries=0;
  * run.meta loader_reads (sync/async_issued/bytes): the poll leg's async_issued
    is at least `--ratio` x the boundary leg's (boundary mode force-completes every
    blocking wait, so the two legs issue nearly the same reads; the ratio is a
    floor against a regression, measured 907/918 at 300 frames);
  * the poll leg's cdvd_forced=0 and cdvd_wait_overruns=0 (the wait parks whole
    frames to the drive's end), cdvd_wait_parks > 0, and wait + poll + boundary
    deliveries + the reads still in flight at the cap (cdvd_deliveries' pending
    field) == async_issued (every read delivered exactly once, none lost);
  * the poll leg's lift counted reply laps (loader_spin_lifts rpc > 0: a module round trip
    completed inside the lap, the IOP's modelled instant still inside the slice, the lap
    continued) and slice-consumed lifts (edge > 0: the round trips' modelled service time
    reached the frame's edge); the per-frame census of the frames in which that reply rule
    decided, run.meta stream_rpc_per_lift_frame=<max>,<mean>,<frames>, holds at least
    `--min-rpc-per-lift-frame` module round trips in one such frame (the reference: ~100
    fit a frame; the floor is a regression guard on the rule continuing laps on replies,
    not the reference figure); the boundary leg charged no round trip (stream_rpc_service=0us,0,
    rpc=0, edge=0, stream_rpc_per_lift_frame=0,0.00,0). The leg-wide census
    stream_rpc_per_frame=<max>,<mean> is REPORTED, not asserted: it counts every STREAM RPC
    of every frame -- its max falls in the FMV phase (the park loop's status pings, present
    in boundary mode too), not in the load;
  * the boot state machine's case word (RAM 0x98c140, case = bits 44..49 of word 0;
    OLD's FUN_00177168 switch) reads >= `--min-case` in the poll
    leg's final image and not below the boundary leg's final image; the first
    boundary whose image reads case 7 is REPORTED from the `--state-every` images
    (not asserted: the drive model's pace, not a constant, decides it).

Both legs run the loader seam's PER-RECORD rule (`--set loader.ring_cache=off`, the
`--ring-cache` option; the Config default has been `on`
since 2026-10-06): every read-count figure above (907 vs 918, the 0.9 ratio, ~83 round trips in a
lift frame) was measured on that seam, and the boundary control leg is the pre-decision
visibility model, which with the ring rule on does not finalize (a load-wait stall near
frame 354 of this leg). Under the
ring rule the poll leg holds every assertion of this file (170 reads, 149 parks, 104
round trips in a lift frame) -- that run is the record's, not this pin's.

SKIP 77 only without the title files. The boundary leg is today's behaviour
(RootPlayPolicyInert pins it bit for bit); this pin measures the toggle's effect.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
BOOT_SM = 0x98C140   # the title's boot state-machine object (SLUS-20909)


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


def boot_case(state_hash, path):
    _, vf, sections = state_hash.parse(path)
    ram = sections[state_hash.TAG_RAM][2]
    w0 = struct.unpack_from("<Q", ram, BOOT_SM)[0]
    return vf, (w0 >> 44) & 0x3F


def run_leg(args, mode, out_dir, log):
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "deterministic", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--state-every", str(args.state_every),
           "--game-files", os.path.abspath(args.game_files),
           # The loader seam's rule, named explicitly on both legs (see the module docstring).
           "--set", "loader.ring_cache=%s" % args.ring_cache]
    for setting in args.settings:
        cmd += ["--set", setting]
    if mode == "poll":
        cmd += ["--set", "cdvd.delivery_visibility=poll"]
    else:
        # The control leg is the pre-decision model, named explicitly: since gate S4
        # (2026-10-03) the Config default is `poll`.
        cmd += ["--set", "cdvd.delivery_visibility=boundary"]
    print("run[%s]: %s" % (mode, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = str(args.timeout)
    proc = subprocess.run(cmd, env=env)
    print("runner[%s] exit=%d" % (mode, proc.returncode))
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--verify-dir", required=True, help="the repo's verify/ (state_hash.py)")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection on both legs (e.g. hle.stream_files=off: the retained "
                         "CDVD-poll arm this pin measures)")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--state-every", type=int, default=60)
    ap.add_argument("--ratio", type=float, default=4.0, help="poll async_issued >= ratio x boundary async_issued")
    ap.add_argument("--min-case", type=int, default=6)
    ap.add_argument("--min-rpc-per-lift-frame", type=int, default=50,
                    help="poll stream_rpc_per_lift_frame max floor (module round trips in one frame the lift's reply rule decided in; measured 83)")
    ap.add_argument("--timeout", type=int, default=240, help="per leg")
    ap.add_argument("--ring-cache", choices=("on", "off"), default="off",
                    help="loader.ring_cache on both legs (off = the per-record seam the figures were measured on)")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    sys.path.insert(0, os.path.abspath(args.verify_dir))
    import state_hash  # noqa: E402

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    dirs, logs, metas = {}, {}, {}
    for mode in ("poll", "boundary"):
        out_dir = tempfile.mkdtemp(prefix="root_cdvd_poll_visibility_%s_" % mode)
        log = out_dir + ".log"
        dirs[mode], logs[mode] = out_dir, log
        rc = run_leg(args, mode, out_dir, log)
        check(rc == 0, "[%s] bounded run exits 0" % mode)
        meta_path = os.path.join(out_dir, "run.meta")
        if not os.path.isfile(meta_path):
            print("FAIL: no run.meta at %s (see %s)" % (meta_path, log))
            return 1
        meta = parse_meta(meta_path)
        metas[mode] = meta
        for key in ("finalized", "frames", "park", "cdvd_visibility", "cdvd_deliveries", "cdvd_poll_deliveries",
                    "cdvd_wait_deliveries", "cdvd_wait_overruns", "cdvd_wait_parks", "cdvd_forced", "cdvd_seeks",
                    "cdvd_boundary_deliveries", "loader_reads", "loader_spin_lifts", "snd_kon"):
            print("run.meta[%s] %s=%s" % (mode, key, meta.get(key, "<absent>")))
        check(meta.get("finalized") == "1", "[%s] finalized=1" % mode)
        check(meta.get("frames") == str(args.frames), "[%s] frames=%d" % (mode, args.frames))
        check(meta.get("park") == "none", "[%s] park=none (the frames cap ended the leg, no guest park)" % mode)
        check(meta.get("cdvd_visibility") == mode, "[%s] cdvd_visibility=%s" % (mode, mode))

    def async_issued(meta):
        try:
            return int(meta.get("loader_reads", "0/0/0").split("/")[1])
        except (ValueError, IndexError):
            return -1

    poll_n, bnd_n = async_issued(metas["poll"]), async_issued(metas["boundary"])
    print("loader_reads async_issued: poll=%d boundary=%d ratio=%.2f" % (poll_n, bnd_n, (poll_n / bnd_n) if bnd_n > 0 else -1))
    check(int(metas["boundary"].get("cdvd_poll_deliveries", "-1")) == 0, "[boundary] cdvd_poll_deliveries=0 (today's behaviour)")
    check(int(metas["poll"].get("cdvd_poll_deliveries", "0")) > 0, "[poll] cdvd_poll_deliveries > 0 (reads landed at the poll)")
    # The census is the model's split (a meter pop whose read was already delivered is not a delivery);
    # under poll the blocking wait delivers at the drive's end -- nothing is forced.
    check(int(metas["poll"].get("cdvd_forced", "-1")) == 0, "[poll] cdvd_forced=0 (the wait delivers at the drive's end, never forced)")
    # A read the modelled drive still holds at the frames cap (run.meta cdvd_deliveries=<delivered>/<pending>)
    # is in flight, not lost: the leg ends mid-read whenever the cap falls inside a long seek.
    try:
        pending = int(metas["poll"].get("cdvd_deliveries", "0/0").split("/")[1])
    except (ValueError, IndexError):
        pending = -1
    delivered = (int(metas["poll"].get("cdvd_wait_deliveries", "0")) + int(metas["poll"].get("cdvd_poll_deliveries", "0")) +
                 int(metas["poll"].get("cdvd_boundary_deliveries", "0")))
    check(pending >= 0 and delivered + pending == poll_n,
          "[poll] wait + poll + boundary deliveries (%d) + in flight at the cap (%d) == async_issued (%d): every read delivered exactly once"
          % (delivered, pending, poll_n))
    check(bnd_n > 0 and poll_n >= args.ratio * bnd_n,
          "[poll] async_issued %d >= %.2f x boundary %d" % (poll_n, args.ratio, bnd_n))
    # Attempt 3: the blocking wait parks whole frames while the drive is busy beyond the frame's
    # edge, so no wait ever overruns the edge, and at least one wait of the Beach load parked.
    check(int(metas["poll"].get("cdvd_wait_overruns", "-1")) == 0, "[poll] cdvd_wait_overruns=0 (waits park to the frame the drive ends in)")
    try:
        parks = int(metas["poll"].get("cdvd_wait_parks", "0/0").split("/")[0])
    except ValueError:
        parks = -1
    check(parks > 0, "[poll] cdvd_wait_parks > 0 (%d frames parked)" % parks)
    check(int(metas["boundary"].get("cdvd_wait_parks", "0/0").split("/")[0]) == 0, "[boundary] cdvd_wait_parks=0 (boundary mode never parks)")
    # BUG-070: a lap with no host-served read in flight runs the IOP's work due inside the frame at the
    # poll and yields when nothing is due (the frame ends; run.meta loader_spin_lifts idle=<n>); the
    # title's load-wait loop therefore never spins a frame to the stall bound. Boundary mode never
    # takes that branch (idle=0, iop_pumps=0).
    def lifts(meta, key):
        for kv in meta.get("loader_spin_lifts", "").split(","):
            if kv.startswith(key + "="):
                try:
                    return int(kv.split("=", 1)[1])
                except ValueError:
                    return -1
        return -1
    check(lifts(metas["poll"], "idle") > 0, "[poll] loader_spin_lifts idle > 0 (the frame ends when nothing is due inside it; %d)" % lifts(metas["poll"], "idle"))
    check(lifts(metas["boundary"], "idle") == 0 and lifts(metas["boundary"], "iop_pumps") == 0,
          "[boundary] loader_spin_lifts idle=0 iop_pumps=0 (the poll-visibility branch is never taken)")
    # The module round trip's cost (HW-1): each STREAM RPC advances the IOP's in-frame clock by the
    # title's measured service time and the lift continues a lap on a reply while the slice has time
    # left (loader_spin_lifts rpc), lifting when the round trips' service reached the edge (edge).
    # The discriminating census is the one over the frames in which that rule decided:
    # stream_rpc_per_lift_frame=<max>,<mean>,<frames> (the module round trips of those frames) and
    # stream_reply_laps_per_frame=<max>,<mean> (the reply-rule decisions of those frames); the floor
    # --min-rpc-per-lift-frame guards the rule continuing laps on replies (a build without the rule
    # has no lift frame at all: 0,0.00,0). stream_rpc_per_frame (every RPC of every frame) is
    # reported only: its max belongs to the FMV phase, where the park loop's status pings pile up
    # in boundary mode too. Boundary mode charges nothing (stream_rpc_service=0us,0; rpc=0 edge=0;
    # no lift frame).
    def census_max(meta, key):
        try:
            return int(meta.get(key, "-1,0").split(",")[0])
        except ValueError:
            return -1
    for key in ("stream_rpc_per_frame", "stream_rpc_per_lift_frame", "stream_reply_laps_per_frame", "iop_inframe_us",
                "cdvd_completions_per_frame", "loader_polls_per_frame", "stream_rpc_service"):
        for mode in ("poll", "boundary"):
            print("run.meta[%s] %s=%s" % (mode, key, metas[mode].get(key, "<absent>")))
    check(lifts(metas["poll"], "rpc") > 0 and lifts(metas["poll"], "edge") > 0,
          "[poll] loader_spin_lifts rpc > 0 and edge > 0 (reply laps continued; the slice's end lifted; %d/%d)"
          % (lifts(metas["poll"], "rpc"), lifts(metas["poll"], "edge")))
    check(census_max(metas["poll"], "stream_rpc_per_lift_frame") >= args.min_rpc_per_lift_frame,
          "[poll] stream_rpc_per_lift_frame max %d >= %d (a frame the reply rule decided in held that many module round trips)"
          % (census_max(metas["poll"], "stream_rpc_per_lift_frame"), args.min_rpc_per_lift_frame))
    check(census_max(metas["poll"], "stream_reply_laps_per_frame") >= 2,
          "[poll] stream_reply_laps_per_frame max %d >= 2 (one frame continued at least one lap on a reply before its end)"
          % census_max(metas["poll"], "stream_reply_laps_per_frame"))
    check(metas["boundary"].get("stream_rpc_service", "") == "0us,0" and lifts(metas["boundary"], "rpc") == 0 and lifts(metas["boundary"], "edge") == 0
          and metas["boundary"].get("stream_rpc_per_lift_frame", "") == "0,0.00,0",
          "[boundary] stream_rpc_service=0us,0 rpc=0 edge=0 stream_rpc_per_lift_frame=0,0.00,0 (no round trip is charged, no lift frame)")

    # The boot state machine: the final image's case, and the first image at case 7.
    final = os.path.join(dirs["poll"], "state_b%d.ps2st" % args.frames)
    images = sorted((int(f[7:-6]), f) for f in os.listdir(dirs["poll"]) if f.startswith("state_b") and f.endswith(".ps2st"))
    case7_at = None
    cases = []
    for n, f in images:
        try:
            vf, case = boot_case(state_hash, os.path.join(dirs["poll"], f))
        except (ValueError, KeyError, struct.error) as e:
            print("  image %s unreadable: %s" % (f, e))
            continue
        cases.append((n, case))
        if case == 7 and case7_at is None:
            case7_at = n
    print("[poll] boot case by image: %s" % ", ".join("b%d:%d" % c for c in cases))
    if cases:
        last_case = cases[-1][1]
        check(last_case >= args.min_case, "[poll] boot case %d >= %d at the final image (b%d)" % (last_case, args.min_case, cases[-1][0]))
    else:
        check(False, "[poll] a --state-every image exists (%s)" % final)
    print("[poll] first image at boot case 7: %s" % ("b%d" % case7_at if case7_at is not None else "none within %d frames (reported, not asserted)" % args.frames))
    bcases = []
    for n, f in sorted((int(f[7:-6]), f) for f in os.listdir(dirs["boundary"]) if f.startswith("state_b") and f.endswith(".ps2st")):
        try:
            bcases.append((n, boot_case(state_hash, os.path.join(dirs["boundary"], f))[1]))
        except (ValueError, KeyError, struct.error):
            pass
    print("[boundary] boot case by image: %s" % ", ".join("b%d:%d" % c for c in bcases))
    if cases and bcases:
        check(cases[-1][1] >= bcases[-1][1], "[poll] boot case %d at the final image not below boundary's %d" % (cases[-1][1], bcases[-1][1]))
    # The title-screen music: a blocking sceSifCallRpc's end function runs at the call, so the
    # title's free-IOP-memory query (cmd 45 + Ship(0)) reads the module's answer and opens the
    # three music stream pairs at the splash; at boot case 7 the module streams MUSIC.MB and keys
    # the voices -- snd_kon > 0 and a non-silent audio window under poll once case 7 was reached.
    # Boundary mode's lanes are frozen (its snd_kon is reported only).
    if case7_at is not None:
        check(int(metas["poll"].get("snd_kon", "0")) > 0, "[poll] snd_kon > 0 once boot case 7 was reached (the title music keyed)")
        check(metas["poll"].get("audio_first_nonsilent_window", "none") != "none",
              "[poll] a non-silent audio window after the case-7 edge")

    if failures == 0:
        for mode in dirs:
            shutil.rmtree(dirs[mode], ignore_errors=True)
            try:
                os.remove(logs[mode])
            except OSError:
                pass
    else:
        print("artifacts kept: %s" % " ".join(list(dirs.values()) + list(logs.values())))
    print("RootCdvdPollVisibility: %s" % ("FAIL" if failures else "ALL PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
