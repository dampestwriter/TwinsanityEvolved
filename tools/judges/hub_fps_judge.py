#!/usr/bin/env python3
"""hub_fps_judge.py -- the frozen judge of the hub's render rate: the hub pad log in PLAY mode (paced,
windowed, scale 1) must present at render_fps p50 >= 55 while Playing, with no ten-frame window averaging more than
16.7 ms per presented frame outside the load windows.

    hub_fps_judge.py --bin BUILD/twinsanityevolved --iso <disc image> [--out DIR] [--fixture fixtures/judges/hub_fps_pal.json]
    hub_fps_judge.py --log RUN_LOG [--fixture ...]            # judge a run already recorded (stderr timestamped per line)

The run: `--play-pad fixtures/padlogs/hub_entry.padlog --skip-movies --trace-state --stop-at 4200` with an empty
`--mc-dir`, NOT `--deterministic` (the pacer runs at the display's 60 Hz: a frame that takes longer than a field is late and
shows as fewer presents per second), `TE_GFX_SUMMARY_EVERY=10` (one `[te.gfx] summary: presents N ...` line per ten
presented frames) and `TE_TICK_LOG_MS=16.7` (the clock logs `[twinsanityevolved] tick: boundary N took X ms` for every
boundary whose work -- the game thread's wall time from the previous boundary's release to this arrival, never the pacer's
park -- took longer than that). Every stderr line is stamped with `time.monotonic` by this script:
the render rate is the wall time between consecutive summary lines over the presents between them.

Judged (the fixture freezes the numbers):
  * render_fps p50 over the ten-present windows in the Playing span (`game state @ boundary 1919: Playing` .. the stop) >= 55
  * ticks > 16.7 ms at boundaries in that span outside the load windows (any boundary within +-30 of a LoadingLevel /
    StartingPlay / Waiting->Playing transition): 0 -- counted from the `tick:` lines by boundary number (a log without
    them, e.g. a binary before this judge, is judged by its ten-present windows instead: a window averaging > 16.7 ms a
    presented frame outside the load spans counts as a slow tick)
  * the run exits 0 at `--stop-at` with no wasm trap; the clock census (`late`, `timeouts`) is reported, not gated
    (the pacer's `late` counts fields the frame missed; it falls with the frame time and is the same measure twice).

Per-pad-log fixtures: fixtures/judges/{jungle_bungle,load_game,hub_quit}_fps_pal.json -- the same thresholds on the
other port pad logs; `extra_args` (runner arguments, `$OUT`/`$ROOT` expanded) and `mc_from` (a card to copy in) in the fixture.
A load state's whole span (entry .. exit, +-margin) is a load window, not only its edges.

Frozen on the prepared-mesh-builder change (platform/nativeee/host/graphics_scene.cpp + the engine's MeshGeometry::Prepare /
BuildPrepared): RED on main 6cd7dc0 (p50 42.7, 321 of 411 windows > 16.7 ms -- every placed/rigid model rebuilt and
re-projected through MeshGeometry::Build per frame), GREEN on the prepared-mesh-builder fix.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "hub_fps_pal.json")

SUMMARY = re.compile(r"^(\d+\.\d+) \[te\.gfx\] summary: presents (\d+) ")
STATE = re.compile(r"^(\d+\.\d+) \[twinsanityevolved\] game state @ boundary (\d+): (\w+) \((\d+)\) <- (\w+)")
CLOCK = re.compile(r"clock: (\d+) boundaries, (\d+) fields, (\d+) parks, (\d+) late, (\d+) timeouts")
TICK = re.compile(r"\] tick: boundary (\d+) took ([\d.]+) ms")
TICKS = re.compile(r"\] ticks: (\d+) measured, (\d+) over the field")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    os.makedirs(mc)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--play-pad", os.path.join(ROOT, fx["padlog"]), "--skip-movies", "--trace-state",
            "--stop-at", str(fx["stop_at"]), "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 240))]
    # per-pad-log fixtures: extra runner arguments (e.g. `--trace-player 30`, `--load-state PATH`); `$OUT`
    # in an argument is the judge's output directory, `$ROOT` the port tree; `mc_from` copies a card into the run's --mc-dir
    # (load_game reads the save the hub log wrote)
    args += [a.replace("$OUT", out).replace("$ROOT", ROOT) for a in fx.get("extra_args", [])]
    if fx.get("mc_from"):
        shutil.rmtree(mc, ignore_errors=True)
        shutil.copytree(fx["mc_from"].replace("$OUT", out).replace("$ROOT", ROOT), mc)
    env = dict(os.environ)
    env["TE_GFX_SUMMARY_EVERY"] = str(fx["summary_every"])
    env["TE_TICK_LOG_MS"] = str(fx["max_ms_per_frame"])
    env.pop("TE_GFX_HEADLESS", None)   # windowed: the check is at scale 1 on the display
    print("hub_fps_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, cwd=ROOT, text=True,
                             errors="replace")
        for line in p.stderr:
            f.write("%.3f %s" % (time.monotonic(), line))
        rc = p.wait()
    print("hub_fps_judge: exit", rc, "->", log, flush=True)
    return log, rc


def judge(log_path, fx, rc=None):
    lines = open(log_path, errors="replace").read().splitlines()
    summaries = []          # (t, presents)
    states = []             # (t, boundary, name, previous)
    ticks = []              # (boundary, ms) -- the slow ticks the clock logged
    ticks_census = None
    clock = None
    trap = None
    stop_reached = False
    for line in lines:
        m = SUMMARY.match(line)
        if m:
            summaries.append((float(m.group(1)), int(m.group(2))))
            continue
        m = TICK.search(line)
        if m:
            ticks.append((int(m.group(1)), float(m.group(2))))
            continue
        m = TICKS.search(line)
        if m:
            ticks_census = (int(m.group(1)), int(m.group(2)))
        m = STATE.match(line)
        if m:
            states.append((float(m.group(1)), int(m.group(2)), m.group(3), m.group(5)))
            continue
        m = CLOCK.search(line)
        if m:
            clock = dict(zip(("boundaries", "fields", "parks", "late", "timeouts"), map(int, m.groups())))
        if "wasm trap" in line:
            trap = line
        if "--stop-at %d reached" % fx["stop_at"] in line:
            stop_reached = True

    problems = []
    measures = {}
    if trap:
        problems.append("trap: " + trap.strip())
    if not stop_reached:
        problems.append("the run did not reach --stop-at %d" % fx["stop_at"])
    if rc not in (None, 0):
        problems.append("exit %d" % rc)

    # the Playing span: the LAST entry into Playing at or before fx["playing_from"] (the hub, after Waiting), to the end
    play_t = None
    play_b = None
    for t, b, name, prev in states:
        if name == "Playing" and b <= fx["playing_from"] + 2:
            play_t, play_b = t, b
    if play_t is None:
        problems.append("no `Playing` state at boundary <= %d" % fx["playing_from"])
    # load windows: +-load_margin boundaries around every load-ish transition -- as boundary ranges (the ticks) and as
    # time spans (the presents' windows; boundary -> t by the trace)
    load_spans = []
    load_ranges = []
    margin = fx["load_margin_boundaries"]
    bt = [(b, t) for t, b, _, _ in states]
    for t, b, name, prev in states:
        if name in fx["load_states"] or (name == "Playing" and prev in fx["load_states"]):
            lo = b - margin
            hi = b + margin
            load_ranges.append((lo, hi))
            # the nearest trace times bracketing lo/hi, else +-margin fields at 60 Hz
            t_lo = max([tt for bb, tt in bt if bb <= lo], default=t - margin / 60.0)
            t_hi = min([tt for bb, tt in bt if bb >= hi], default=t + margin / 60.0)
            load_spans.append((t_lo, t_hi))
    # ... and the WHOLE span of a load state (entry .. the next state change), +-margin: a LoadingTitle/LoadingLevel that
    # lasts 150 boundaries is a load window throughout, not only at its edges (hub_quit's 20.8 ms tick at
    # 2357 inside LoadingTitle 2263..2419). The hub fixture's load states (Waiting 1903..1918) already fit in +-30.
    for (t0, b0, name, _), (t1, b1, _, _) in zip(states, states[1:]):
        if name in fx["load_states"] and b1 - b0 > margin:
            load_ranges.append((b0 - margin, b1 + margin))
            load_spans.append((t0 - margin / 60.0, t1 + margin / 60.0))

    windows = []   # (t_end, ms_per_frame, fps)
    for (t0, p0), (t1, p1) in zip(summaries, summaries[1:]):
        if play_t is None or t0 < play_t:
            continue
        frames = p1 - p0
        # two summaries can carry the same timestamp when the host stalls the process and its stderr flushes land
        # together (the jungle run): a zero-length window has no rate -- skip it rather than divide by 0
        if frames <= 0 or t1 <= t0:
            continue
        ms = (t1 - t0) * 1000.0 / frames
        windows.append((t1, ms, 1000.0 / ms))
    measures["windows"] = len(windows)
    if windows:
        fps = sorted(w[2] for w in windows)
        ms = sorted(w[1] for w in windows)
        measures["render_fps_p50"] = round(statistics.median(fps), 1)
        measures["render_fps_min"] = round(fps[0], 1)
        measures["render_fps_max"] = round(fps[-1], 1)
        measures["ms_per_frame_p50"] = round(statistics.median(ms), 2)
        measures["ms_per_frame_max"] = round(ms[-1], 2)
        slow = [w for w in windows if w[1] > fx["max_ms_per_frame"]]
        outside = [w for w in slow if not any(lo <= w[0] <= hi for lo, hi in load_spans)]
        measures["windows_over_%s_ms" % fx["max_ms_per_frame"]] = len(slow)
        measures["windows_over_%s_ms_outside_load" % fx["max_ms_per_frame"]] = len(outside)
        if measures["render_fps_p50"] < fx["min_render_fps_p50"]:
            problems.append("render_fps p50 %.1f < %s" % (measures["render_fps_p50"], fx["min_render_fps_p50"]))
        if len(windows) < fx["min_windows"]:
            problems.append("only %d windows measured (< %d)" % (len(windows), fx["min_windows"]))
        # the slow ticks: by boundary when the clock logged them, else the windows
        if ticks_census is not None and play_b is not None:
            in_span = [(b, ms) for b, ms in ticks if play_b <= b <= fx["stop_at"] and ms > fx["max_ms_per_frame"]]
            slow_ticks = [(b, ms) for b, ms in in_span if not any(lo <= b <= hi for lo, hi in load_ranges)]
            measures["ticks_measured"] = ticks_census[0]
            measures["ticks_over_%s_ms_in_playing" % fx["max_ms_per_frame"]] = len(in_span)
            measures["ticks_over_%s_ms_outside_load" % fx["max_ms_per_frame"]] = len(slow_ticks)
            if slow_ticks:
                measures["slow_ticks"] = ["%d:%.1f" % t for t in sorted(slow_ticks, key=lambda t: -t[1])[:12]]
            if len(slow_ticks) > fx["max_slow_windows_outside_load"]:
                problems.append("%d ticks > %s ms outside the load windows (max %d); worst boundary %d at %.1f ms" %
                                (len(slow_ticks), fx["max_ms_per_frame"], fx["max_slow_windows_outside_load"],
                                 max(slow_ticks, key=lambda t: t[1])[0], max(ms for _, ms in slow_ticks)))
        elif len(outside) > fx["max_slow_windows_outside_load"]:
            problems.append("%d windows > %s ms outside the load windows (max %d); worst %.1f ms (no tick lines: judged by the windows)" %
                            (len(outside), fx["max_ms_per_frame"], fx["max_slow_windows_outside_load"],
                             max(w[1] for w in outside)))
    else:
        problems.append("no presents measured in the Playing span")
    if clock:
        measures["clock"] = clock
        if clock["boundaries"]:
            measures["clock_late_pct"] = round(100.0 * clock["late"] / clock["boundaries"], 1)
    return problems, measures, windows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--log")
    ap.add_argument("--out")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--series", action="store_true", help="print every window (t, ms/frame, fps)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    rc = None
    if a.log:
        log = a.log
    else:
        if not (a.bin and a.iso):
            ap.error("--bin and --iso (or --log)")
        out = a.out or tempfile.mkdtemp(prefix="hub_fps_")
        log, rc = run(a.bin, a.iso, out, fx)
    problems, measures, windows = judge(log, fx, rc)
    if a.series:
        for t, ms, fps in windows:
            print("  %.3f  %6.2f ms  %5.1f fps" % (t, ms, fps))
    print("hub_fps_judge:", json.dumps(measures, sort_keys=True))
    for p in problems:
        print("  RED:", p)
    if problems:
        print("hub_fps_judge: RED")
        sys.exit(1)
    print("hub_fps_judge: GREEN (render_fps p50 %.1f >= %s; %d slow ticks outside load; %d windows)" %
          (measures["render_fps_p50"], fx["min_render_fps_p50"],
           measures.get("ticks_over_%s_ms_outside_load" % fx["max_ms_per_frame"],
                        measures["windows_over_%s_ms_outside_load" % fx["max_ms_per_frame"]]), measures["windows"]))


if __name__ == "__main__":
    main()
