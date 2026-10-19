#!/usr/bin/env python3
"""RootHighFrameRate (video.render_rate=display): the game's logic stays at the 60 Hz
tick while the presenter renders at the DISPLAY's rate -- here a 120 Hz display simulated by the
presenter clock (`--set harness.display_hz=120`) -- with the camera interpolated between the two
latest ticks for every presented frame (CameraTrack; the transform source is the title's
RenderView::toCamera published by the owned projection entry after every tick's Update).

One wall-paced windowed replay (`--harness-window --inputs <padlog>`, the beach route) with
`--set video.render_rate=display --set harness.display_hz=120`. Judged (every line read from
run.meta and <leg>/camera.trace):
  * runner exit 0, finalized=1, park=none, video=gl, frames=N.
  * display_rows render_rate=display present_hz=120; presents >= 1.8 x the frames (two presents
    per game tick at 120 Hz).
  * the clock contract holds at the display rate: game_hz 59.5..60.5 in every in-loop window
    (fields > boundaries = an EE-own-time frame, tolerated up to --max-own-time-windows and
    printed), field_hz 58..62, gs_queue frame_waits=0, audio dropped=0 underruns=0 (the
    render rate never touches the game's speed).
  * render_fps >= 110 in every in-loop window (120 presents per second, each noted by the presenter).
  * NO JUDDER on the camera pan: camera.trace holds one line per presented frame (wall_ns tick
    phase x y z xn yn zn dup) -- the interpolated position and the latest TICK's own position
    (what a tick-rate duplicate would show). Over the trace's busiest pan (the --pan-window
    consecutive presents with the largest total camera travel, >= --pan-min units) the
    INTERPOLATED per-present deltas have a coefficient of variation (std / mean) <= --cv-bound
    (0.5: steps of a steady pan at 120 Hz are within half their mean of each other) and the
    pan's progress along its principal direction is monotone (no backward step beyond 2 %% of the
    mean step); the TICK-RATE columns of the SAME presents -- the red-before, duplicates at the
    display rate -- read a CV >= 2 x the interpolated one (every other step is zero), proving
    the measurement discriminates. hfr_camera interpolated >= 40 %% of the presents.
  * THE PIXELS (A3): <leg>/present.trace holds one line per picture the WINDOW received
    (`wall_ns kind tick phase x y z`; kind 1 = a frame RE-VIEWED by FrameReview at the interpolated
    camera, kind 0 = the tick's own frame -- shown while the consumer's cameras are stale). BLOCKING:
    over the busiest continuous pan whose pictures are ALL re-viewed (the consumer kept up there)
    the presented camera positions' per-present deltas have a CV <= --cv-bound; run.meta hfr_review
    reads replays > 0, faults = 0, stale = 0 (every replay issued the frame's complete command
    list). REPORT: the busiest pan overall (the beach WALK when continuous) with its re-viewed share
    -- judged only when >= 90 %% re-viewed; below that the consumer lagged the stream (the level
    route's frame cost) and the window showed tick frames: the budget gap, named, not faked. The
    red-before is the previous binary: no present.trace at all (FAIL "present.trace") -- its pixels
    were the tick-rate duplicates whose step pattern the camera.trace tick columns show (CV ~1.06
    on the record's run; 1.43..1.69 on this session's walk).
--selftest judges synthetic traces (a steady pan interpolated vs duplicated; a jittery
interpolation; a reversing track; a trace without motion; a present trace steady / duplicated /
absent) and canned run.meta rows.
SKIP 77 without the title files / the binary, or when the window is a stub (video=stub).

The re-view re-projects the recorded screen-space vertices of the camera-projected VU1 families
(the world geometry) through the interpolated camera; screen-space draws (HUD, text, sprites, the
owned 2D passes) and any unstamped draw (the model/raw route) draw at the tick. Objects' own
motion steps at the tick (no per-object matrices on the stream yet); the camera is smooth.
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")


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


def series(text):
    out = []
    for part in (text or "").split(","):
        part = part.strip()
        if part.isdigit():
            out.append(int(part))
    return out


def read_trace(path):
    """[(wall_ns, tick, phase, (x,y,z), (xn,yn,zn), dup)] or []."""
    rows = []
    if not os.path.isfile(path):
        return rows
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 10:
                continue
            try:
                rows.append((int(parts[0]), int(parts[1]), float(parts[2]),
                             (float(parts[3]), float(parts[4]), float(parts[5])),
                             (float(parts[6]), float(parts[7]), float(parts[8])), int(parts[9])))
            except ValueError:
                continue
    return rows


def read_present_trace(path):
    """[(wall_ns, kind, tick, phase, (x,y,z))] or []."""
    rows = []
    if not os.path.isfile(path):
        return rows
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 7:
                continue
            try:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2]), float(parts[3]),
                             (float(parts[4]), float(parts[5]), float(parts[6]))))
            except ValueError:
                continue
    return rows


def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def travel(points):
    return sum(dist(points[i], points[i - 1]) for i in range(1, len(points)))


def cv(values):
    n = len(values)
    if n == 0:
        return float("inf")
    mean = sum(values) / n
    if mean <= 0:
        return float("inf")
    var = sum((v - mean) ** 2 for v in values) / n
    return math.sqrt(var) / mean


def tick_steps_of(rows):
    """The latest-tick positions' step per TICK change within rows (one entry per distinct tick)."""
    steps = []
    last_tick, last_pos = None, None
    for r in rows:
        if r[1] != last_tick:
            if last_pos is not None:
                steps.append(dist(r[4], last_pos))
            last_tick, last_pos = r[1], r[4]
    return steps


def analyse_pan(rows, window, pan_min, cut_ratio=8.0, moving_share=0.8):
    """The busiest CONTINUOUS pan of `window` consecutive presents: the camera moved on >= moving_share
    of the window's ticks and no tick step exceeds cut_ratio x the window's median moving step (a
    camera cut / load teleport is not a pan). Among those, the largest travel; None when no window
    travels pan_min units."""
    if len(rows) < window + 1:
        return None
    best, best_travel = None, -1.0
    interp = [r[3] for r in rows]
    prefix = [0.0]
    for i in range(1, len(interp)):
        prefix.append(prefix[-1] + dist(interp[i], interp[i - 1]))
    for start in range(0, len(rows) - window, max(1, window // 8)):
        t = prefix[start + window] - prefix[start]
        if t <= best_travel or t < pan_min:
            continue
        ts = tick_steps_of(rows[start:start + window + 1])
        moving = sorted(x for x in ts if x > 0)
        if not ts or len(moving) < moving_share * len(ts):
            continue
        median = moving[len(moving) // 2]
        if max(ts) > cut_ratio * median:
            continue
        best, best_travel = start, t
    if best is None:
        return None
    seg = rows[best:best + window + 1]
    interp_pts = [r[3] for r in seg]
    tick_pts = [r[4] for r in seg]
    interp_steps = [dist(interp_pts[i], interp_pts[i - 1]) for i in range(1, len(interp_pts))]
    tick_steps = [dist(tick_pts[i], tick_pts[i - 1]) for i in range(1, len(tick_pts))]
    # Principal direction = start -> end of the interpolated track; progress along it.
    d = [interp_pts[-1][i] - interp_pts[0][i] for i in range(3)]
    norm = math.sqrt(sum(c * c for c in d)) or 1.0
    d = [c / norm for c in d]
    progress = [sum((p[i] - interp_pts[0][i]) * d[i] for i in range(3)) for p in interp_pts]
    mean_step = sum(interp_steps) / len(interp_steps)
    backward = max(progress[i - 1] - progress[i] for i in range(1, len(progress)))
    return {
        "start": best, "wall_s": (seg[-1][0] - seg[0][0]) / 1e9, "travel": best_travel,
        "interp_cv": cv(interp_steps), "tick_cv": cv(tick_steps), "mean_step": mean_step,
        "backward": backward, "zero_tick_steps": sum(1 for s in tick_steps if s == 0.0),
        "dups": sum(r[5] for r in seg), "ticks": seg[-1][1] - seg[0][1],
    }


def analyse_presented(present_rows, window, pan_min, reviewed_only=False):
    """The pixels' pan: the busiest continuous pan of the PRESENTED positions (the same rule as
    analyse_pan over camera.trace rows shaped (wall, tick, phase, pos, pos, 0)); adds the share of
    re-viewed presents and the clamped phases (duplicates) in the window. reviewed_only: among the
    windows whose pictures are ALL re-viewed (kind 1) -- where the consumer kept up."""
    shaped = [(r[0], r[2], r[3], r[4], r[4], 0) for r in present_rows]
    if reviewed_only:
        best, best_travel = None, -1.0
        for start in range(0, len(present_rows) - window, max(1, window // 8)):
            seg = present_rows[start:start + window + 1]
            if any(r[1] != 1 for r in seg):
                continue
            pan = analyse_pan(shaped[start:start + window + 1], window, pan_min)
            if pan is not None and pan["travel"] > best_travel:
                best, best_travel = start, pan["travel"]
        if best is None:
            return None
        pan = analyse_pan(shaped[best:best + window + 1], window, pan_min)
        pan["start"] = best
    else:
        pan = analyse_pan(shaped, window, pan_min)
    if pan is None:
        return None
    seg = present_rows[pan["start"]:pan["start"] + window + 1]
    pan["reviewed_share"] = sum(1 for r in seg if r[1] == 1) / float(len(seg))
    pan["clamped"] = sum(1 for r in seg if r[3] >= 1.0)
    dts = [(seg[i][0] - seg[i - 1][0]) / 1e6 for i in range(1, len(seg))]
    pan["dt_ms_mean"] = sum(dts) / len(dts)
    pan["dt_ms_max"] = max(dts)
    return pan


def judge(returncode, meta, frames, rows, pan_window=240, pan_min=0.5, cv_bound=0.5, max_own_time_windows=3,
          min_render_fps=110, present_rows=None):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "runner exit 0 (got %d)" % returncode)
    check(meta.get("finalized") == "1", "run.meta finalized=1 (got %s)" % meta.get("finalized"))
    check(meta.get("frames") == str(frames), "run.meta frames=%d (got %s)" % (frames, meta.get("frames")))
    check(meta.get("park") == "none", "run.meta park=none (got %s)" % meta.get("park", "<absent>"))
    check(meta.get("video") == "gl", "run.meta video=gl (got %s)" % meta.get("video"))
    dr = census(meta.get("display_rows"))
    check(dr.get("render_rate") == "display" and dr.get("present_hz") == "120",
          "display_rows render_rate=display present_hz=120 (got %s)" % (meta.get("display_rows") or "<absent>"))
    presents = int(dr.get("presents", "0") or 0)
    check(presents >= 1.8 * frames, "presents %d >= 1.8 x frames %d (two presents per game tick at 120 Hz)" % (presents, frames))
    gq = census(meta.get("gs_queue"))
    check(gq.get("frame_waits") == "0", "rule 2: gs_queue frame_waits=0 (got %s)" % gq.get("frame_waits", "<absent>"))
    ar = census(meta.get("audio_ring"))
    check(ar.get("dropped") == "0" and ar.get("underruns") == "0",
          "rule 4: audio_ring dropped=0 underruns=0 (got dropped=%s underruns=%s)" % (ar.get("dropped"), ar.get("underruns")))
    game_hz, field_hz, render_fps = series(meta.get("game_hz")), series(meta.get("field_hz")), series(meta.get("render_fps"))
    n = min(len(game_hz), len(field_hz), len(render_fps))
    rows_hz = list(zip(game_hz[:n], field_hz[:n], render_fps[:n]))
    in_loop = [rows_hz[i] for i in range(1, len(rows_hz) - 1)
               if all(r[0] >= 30 for r in (rows_hz[i - 1], rows_hz[i], rows_hz[i + 1]))]
    check(len(in_loop) >= 5, "the title ran its frame loop in >= 5 windows (%d)" % len(in_loop))
    bad_fields = [f for _, f, _ in in_loop if not 58 <= f <= 62]
    check(not bad_fields, "rule 3: field_hz 58..62 in every in-loop window (out of range: %s)" % (bad_fields or "none"))
    own_time = [(g, f) for g, f, _ in in_loop if f > g]
    out_of_band = [g for g, f, _ in in_loop if not (59.5 <= g <= 60.5) and not f > g]
    check(not out_of_band, "game_hz 60 at the display rate: 59.5..60.5 in every in-loop window without an EE-own-time frame (out of band: %s)" % (out_of_band or "none"))
    check(len(own_time) <= max_own_time_windows, "EE_OWN_TIME windows = %d <= %d: %s" % (len(own_time), max_own_time_windows, own_time or "none"))
    low_fps = [r for _, _, r in in_loop if r < min_render_fps]
    check(not low_fps, "render_fps >= %d in every in-loop window (the presenter at 120 Hz; low: %s)" % (min_render_fps, low_fps or "none"))
    if in_loop:
        checks.append((True, "REPORT in-loop windows=%d game_hz min/max=%d/%d render_fps min/max=%d/%d" % (
            len(in_loop), min(g for g, _, _ in in_loop), max(g for g, _, _ in in_loop),
            min(r for _, _, r in in_loop), max(r for _, _, r in in_loop))))
    hc = census(meta.get("hfr_camera"))
    samples, hpresents, interpolated = int(hc.get("samples", "0") or 0), int(hc.get("presents", "0") or 0), int(hc.get("interpolated", "0") or 0)
    check(samples > 0, "hfr_camera samples > 0 (the projection entry published the camera; got %s)" % (meta.get("hfr_camera") or "<absent>"))
    check(hpresents > 0 and interpolated >= 0.4 * hpresents,
          "hfr_camera interpolated %d >= 40%% of presents %d (phases strictly inside the tick)" % (interpolated, hpresents))
    unsampled = int(hc.get("unsampled", "0") or 0)
    # The census is taken before the presenter stops: the presents of the shutdown (up to a
    # second of them on a loaded machine) follow it in the trace.
    expected = hpresents - unsampled
    check(len(rows) > 0 and expected - 2 <= len(rows) <= expected + 130,
          "camera.trace has one line per present since the first camera sample (%d lines, %d presents - %d unsampled; shutdown presents tolerated)" % (len(rows), hpresents, unsampled))
    pan = analyse_pan(rows, pan_window, pan_min)
    check(pan is not None, "a camera pan of >= %.2f units within %d presents exists in the trace (continuous: >= 80%% of ticks moving, no cut)" % (pan_min, pan_window))
    if pan:
        checks.append((True, "REPORT pan: presents %d.. (%d ticks, %.2f s wall) travel %.2f mean step %.4f; interpolated CV %.3f, tick-rate CV %.3f (zero steps %d/%d, dups %d), max backward %.4f"
                       % (pan["start"], pan["ticks"], pan["wall_s"], pan["travel"], pan["mean_step"], pan["interp_cv"], pan["tick_cv"],
                          pan["zero_tick_steps"], pan_window, pan["dups"], pan["backward"])))
        check(pan["interp_cv"] <= cv_bound, "no judder: the interpolated per-present camera deltas' CV %.3f <= %.2f" % (pan["interp_cv"], cv_bound))
        check(pan["backward"] <= 0.02 * pan["mean_step"], "monotone: no backward step along the pan beyond 2%% of the mean step (max %.5f, step %.5f)" % (pan["backward"], pan["mean_step"]))
        check(pan["tick_cv"] >= 2 * pan["interp_cv"], "red-before discriminated: the tick-rate duplicates' CV %.3f >= 2 x the interpolated %.3f" % (pan["tick_cv"], pan["interp_cv"]))
    # The pixels: the pictures the window received, re-viewed at the interpolated camera.
    present_rows = present_rows or []
    hr = census(meta.get("hfr_review"))
    replays = int(hr.get("replays", "0") or 0)
    check(len(present_rows) > 0, "present.trace has the window's presents (%d lines; absent on the previous binary = the red-before)" % len(present_rows))
    check(replays > 0 and hr.get("faults") == "0" and hr.get("stale") == "0",
          "hfr_review replays > 0, faults=0, stale=0 (got %s)" % (meta.get("hfr_review") or "<absent>"))
    # (a) BLOCKING -- the mechanism: over the busiest pan whose pictures are ALL re-viewed (the
    # consumer kept up with the stream there), the presented deltas are smooth.
    rpan = analyse_presented(present_rows, pan_window, pan_min, reviewed_only=True) if present_rows else None
    check(rpan is not None, "a continuous pan of >= %.2f units within %d presents exists whose pictures are all re-viewed" % (pan_min, pan_window))
    if rpan:
        checks.append((True, "REPORT pixels (re-viewed pan): presents %d.. (%d ticks) travel %.2f mean step %.4f; presented CV %.3f, clamped phases %d, dt ms mean %.2f max %.2f"
                       % (rpan["start"], rpan["ticks"], rpan["travel"], rpan["mean_step"], rpan["interp_cv"], rpan["clamped"], rpan["dt_ms_mean"], rpan["dt_ms_max"])))
        check(rpan["interp_cv"] <= cv_bound, "no judder in the re-viewed PIXELS: the presented camera deltas' CV %.3f <= %.2f" % (rpan["interp_cv"], cv_bound))
    # (b) REPORT -- the budget: the busiest pan overall (the beach WALK when it qualifies) with its
    # re-viewed share and clamped phases; stale_cameras from hfr_review = the edges the consumer's
    # lag turned into tick frames. The walk is smooth only where the consumer has the slack for two
    # re-views per tick (the level route's frame cost: /1d).
    ppan = analyse_presented(present_rows, pan_window, pan_min) if present_rows else None
    if ppan:
        checks.append((True, "REPORT pixels (busiest pan): presents %d.. (%d ticks) travel %.2f mean step %.4f; presented CV %.3f, re-viewed share %.2f, clamped phases %d, dt ms mean %.2f max %.2f; stale_cameras=%s tick_presents=%s"
                       % (ppan["start"], ppan["ticks"], ppan["travel"], ppan["mean_step"], ppan["interp_cv"], ppan["reviewed_share"],
                          ppan["clamped"], ppan["dt_ms_mean"], ppan["dt_ms_max"], hr.get("stale_cameras", "?"), hr.get("tick_presents", "?"))))
        if ppan["reviewed_share"] >= 0.9:
            check(ppan["interp_cv"] <= cv_bound, "no judder in the busiest pan's PIXELS (all re-viewed): CV %.3f <= %.2f" % (ppan["interp_cv"], cv_bound))
        else:
            checks.append((True, "REPORT busiest pan NOT judged: re-viewed share %.2f < 0.9 (the consumer lagged: tick frames shown)" % ppan["reviewed_share"]))
    return checks


def synthetic_trace(presents=600, hz=120, tick_hz=60, speed=1.0, jitter=0.0, reverse_at=None, moving=True):
    rows = []
    for i in range(presents):
        t = i / hz
        tick = int(t * tick_hz) + 1
        phase = (t * tick_hz) % 1.0
        tick_pos = (tick - 1) / tick_hz * speed if moving else 0.0
        prev_pos = (tick - 2) / tick_hz * speed if moving else 0.0
        p = prev_pos + (tick_pos - prev_pos) * phase
        if jitter:
            p += jitter * ((i * 7919) % 13 - 6) / 6.0
        if reverse_at is not None and (i // reverse_at) % 2 == 1:
            # an oscillating track: every `reverse_at` presents the pan runs backward
            k = i // reverse_at
            p = 2 * (k * reverse_at / hz * speed) - p
        dup = 1 if rows and rows[-1][1] == tick else 0
        rows.append((int(t * 1e9), tick, phase, (p, 0.0, 0.0), (tick_pos, 0.0, 0.0), dup))
    return rows


def selftest():
    fails = 0
    meta = {"finalized": "1", "frames": "7000", "park": "none", "video": "gl",
            "display_rows": "scale_setting=1,scale=1,scale_requests=0,scale_applied=0,scale_async=0,scale_inline=0,vsync=0,fullscreen=0,aspect=0,render_rate=display,present_hz=120,presents=14000",
            "gs_queue": "frames=7000,frame_waits=0,frame_skip=1", "audio_ring": "pushed=1,dropped=0,pulled=1,underruns=0",
            "game_hz": ",".join(["60"] * 12), "field_hz": ",".join(["60"] * 12), "render_fps": ",".join(["120"] * 12),
            "hfr_camera": "samples=7000,presents=14000,interpolated=13000,duplicates=7000,unsampled=0,phase_mean_x1000=500,trace=on"}
    meta["hfr_review"] = "recording=1,replays=13000,refused=0,faults=0,stale=0,reviewed_draws=1,fixed_draws=1"
    good = synthetic_trace(presents=14000)
    good_present = [(a, 1, b, c, d) for a, b, c, d, e, f in good]
    dup_present = [(a, 1, b, c, e) for a, b, c, d, e, f in good]
    r = judge(0, meta, 7000, good, present_rows=good_present)
    ok = all(c for c, _ in r)
    print("selftest steady pan interpolated at 120 Hz passes: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    for c, w in r:
        if w.startswith("REPORT"):
            print("  " + w)
    # red: the interpolated columns equal the tick-rate ones (duplicates at the display rate)
    dup = [(a, b, c, e, e, f) for a, b, c, d, e, f in good]
    r = judge(0, meta, 7000, dup, present_rows=dup_present)
    bad = [w for c, w in r if not c]
    ok = any(w.startswith("no judder:") for w in bad) and any(w.startswith("red-before") for w in bad) \
        and any(w.startswith("no judder in the re-viewed PIXELS") for w in bad)
    print("selftest tick-rate duplicates fail the judder and discrimination checks (camera and pixels): %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: the previous binary -- no present.trace, no review census
    r = judge(0, dict(meta, hfr_review=""), 7000, good, present_rows=[])
    bad = [w for c, w in r if not c]
    ok = any(w.startswith("present.trace") for w in bad) and any(w.startswith("hfr_review") for w in bad) \
        and any(w.startswith("a continuous pan") and "all re-viewed" in w for w in bad)
    print("selftest no present.trace (the previous binary) fails the pixel checks: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: the pictures are mostly the tick's frames (the review rarely armed)
    mostly_tick = [(a, 0 if i % 4 else 1, b, c, d) for i, (a, b, c, d, e, f) in enumerate(good)]
    r = judge(0, meta, 7000, good, present_rows=mostly_tick)
    ok = any(w.startswith("a continuous pan") and "all re-viewed" in w for c, w in r if not c)
    print("selftest mostly tick presents fail (no all-re-viewed pan): %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: a jittery interpolation
    r = judge(0, meta, 7000, synthetic_trace(presents=14000, jitter=0.05), present_rows=good_present)
    ok = any(w.startswith("no judder:") for c, w in r if not c)
    print("selftest jittery interpolation fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: a reversing track fails monotonicity
    r = judge(0, meta, 7000, synthetic_trace(presents=14000, reverse_at=60), present_rows=good_present)
    ok = any(w.startswith("monotone") for c, w in r if not c)
    print("selftest reversing pan fails monotone: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: no camera motion
    r = judge(0, meta, 7000, synthetic_trace(presents=14000, moving=False), present_rows=good_present)
    ok = any(w.startswith("a camera pan") for c, w in r if not c)
    print("selftest motionless trace fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: the presenter stayed at 60 (render_rate=60) -- presents, present_hz and render_fps
    m60 = dict(meta, display_rows=meta["display_rows"].replace("render_rate=display,present_hz=120,presents=14000", "render_rate=60,present_hz=60,presents=7000"),
               render_fps=",".join(["60"] * 12))
    r = judge(0, m60, 7000, good, present_rows=good_present)
    ok = sum(1 for c, _ in r if not c) == 3
    print("selftest tick-rate presenter fails 3 checks: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: the game sped up / slowed down at the display rate
    slow = ",".join(["60"] * 6 + ["57"] + ["60"] * 5)   # boundaries AND fields short: the game slowed
    r = judge(0, dict(meta, game_hz=slow, field_hz=slow), 7000, good, present_rows=good_present)
    ok = any(w.startswith("game_hz 60") for c, w in r if not c)
    print("selftest game_hz out of band fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    # red: a renderer wait
    r = judge(0, dict(meta, gs_queue="frames=7000,frame_waits=12,frame_skip=1"), 7000, good, present_rows=good_present)
    ok = any(w.startswith("rule 2") for c, w in r if not c)
    print("selftest frame_waits fails: %s" % ("PASS" if ok else "FAIL")); fails += not ok
    print("root_high_frame_rate selftest: %s" % ("PASS" if fails == 0 else "FAIL (%d)" % fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--padlog")
    ap.add_argument("--frames", type=int, default=7000)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--display-hz", type=int, default=120)
    ap.add_argument("--pan-window", type=int, default=240)
    ap.add_argument("--pan-min", type=float, default=0.5)
    ap.add_argument("--cv-bound", type=float, default=0.5)
    ap.add_argument("--max-own-time-windows", type=int, default=3)
    ap.add_argument("--min-render-fps", type=int, default=110)
    ap.add_argument("--keep", help="copy run.meta, camera.trace and present.trace here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner", "padlog"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    out_dir = tempfile.mkdtemp(prefix="root_high_frame_rate_")
    mc_dir = os.path.join(out_dir, "mc")
    leg = os.path.join(out_dir, "leg")
    os.makedirs(mc_dir)
    os.makedirs(leg)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary, "--profile", "default", "--harness-window",
           "--harness-out", leg, "--frames", str(args.frames), "--inputs", os.path.abspath(args.padlog),
           "--game-files", os.path.abspath(args.game_files), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.log_dir=" + os.path.join(out_dir, "logs"), "--set", "ui.gui_open=off",
           "--set", "video.render_rate=display", "--set", "harness.display_hz=%d" % args.display_hz,
           # The simulated 120 Hz display: the real swap interval would clamp the presenter to the
           # monitor's rate (video.vsync is ON by default).
           "--set", "video.vsync=off"]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(leg, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(leg, "run.meta")
    meta = parse_meta(meta_path)
    for key in ("frames", "finalized", "park", "video", "wall_ms", "gs_queue", "frames_dropped", "audio_ring", "pace_late",
                "game_hz", "field_hz", "render_fps", "display_rows", "hfr_camera", "hfr_review"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    trace_path = os.path.join(leg, "camera.trace")
    rows = read_trace(trace_path)
    present_path = os.path.join(leg, "present.trace")
    present_rows = read_present_trace(present_path)
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        for f in (meta_path, trace_path, present_path):
            if os.path.isfile(f):
                shutil.copy(f, os.path.join(args.keep, os.path.basename(f)))
    if proc.returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77
    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, rows, args.pan_window, args.pan_min, args.cv_bound,
                          args.max_own_time_windows, args.min_render_fps, present_rows):
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += not ok
    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("RootHighFrameRate: %s" % ("ALL PASS" if failures == 0 else "%d FAIL (artifacts: %s, %s)" % (failures, out_dir, log)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
