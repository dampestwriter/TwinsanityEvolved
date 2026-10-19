#!/usr/bin/env python3
"""title_frame_judge.py -- the presented-frame judge: a PRESENTED frame is neither black nor a flat clear colour.

The runner writes presented frames as binary PPM (TE_GFX_DUMP_DIR / TE_GFX_DUMP_EVERY / TE_GFX_DUMP_AT, platform/nativeee/
host/graphics.cpp) through the engine's presentation capture (ps2e_draw_arm_capture), so what is judged is what the window
showed. The judge reads one frame (or the newest frame in a directory) and measures:

  luminance_mean      mean of 0.299 R + 0.587 G + 0.114 B over every pixel (0..255)
  dark_share          share of pixels with luminance < --dark (default 16)
  top_colour_share    share of the single most common RGB value (the clear colour or the flat background)
  distinct_colours    number of distinct RGB values (capped at --max-distinct for speed)

and passes when luminance_mean >= --min-luminance (default 8), dark_share <= --max-dark (default 0.98) and
top_colour_share <= --max-top (default 0.90). With --clear R,G,B the share of THAT colour is also reported and bounded by
--max-top (the frame must show more than its clear). With --summary-log FILE the newest "[te.gfx] summary:" line is parsed
and the frame must have had draws besides StartFrame's clear (sprites + turned + strips + text quads >= --min-draws, default
1) -- the arena-judge pattern's draws-per-frame floor.

With --frame N a directory's frame_N.ppm is judged instead of its newest (the legal screen is frame 35 of a dump-every-1
run; the title's first frames follow the loading screen). With --state-log FILE (the runner's stderr from a --trace-state
run) the game must have reached --require-state (default Title) and held it for --min-state-boundaries boundaries
(default 50: the title draws "press start" after 50 frames); the state's boundary range is reported.

Exit 0 = green (one JSON line of the measures), 1 = red, 2 = no frame.
"""
import argparse
import collections
import json
import os
import re
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n<bytes>  (comments not written by the runner; tolerated)
    tokens = []
    pos = 0
    while len(tokens) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    pos += 1
    if tokens[0] != b"P6":
        raise ValueError(f"{path}: not a binary PPM")
    w, h, maxval = int(tokens[1]), int(tokens[2]), int(tokens[3])
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval} unsupported")
    px = data[pos:pos + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError(f"{path}: truncated ({len(px)} of {w * h * 3} bytes)")
    return w, h, px


def measure(w, h, px, dark, max_distinct, clear):
    n = w * h
    counts = collections.Counter()
    lum_sum = 0.0
    dark_count = 0
    step = 3
    for i in range(0, n * 3, step):
        r, g, b = px[i], px[i + 1], px[i + 2]
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        lum_sum += lum
        if lum < dark:
            dark_count += 1
        counts[(r, g, b)] += 1
    top_colour, top_count = counts.most_common(1)[0]
    out = {
        "width": w, "height": h,
        "luminance_mean": round(lum_sum / n, 3),
        "dark_share": round(dark_count / n, 4),
        "top_colour": list(top_colour),
        "top_colour_share": round(top_count / n, 4),
        "distinct_colours": min(len(counts), max_distinct),
    }
    if clear is not None:
        out["clear_colour"] = list(clear)
        out["clear_colour_share"] = round(counts.get(tuple(clear), 0) / n, 4)
    return out


def newest_frame(path, frame=None):
    if os.path.isdir(path):
        if frame is not None:
            p = os.path.join(path, f"frame_{frame:06d}.ppm")
            return p if os.path.exists(p) else None
        frames = sorted(f for f in os.listdir(path) if f.endswith(".ppm"))
        if not frames:
            return None
        return os.path.join(path, frames[-1])
    return path if os.path.exists(path) else None


STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\) <- (\w+) \((\d+)\), next (\w+) \((\d+)\)")


def state_spans(path):
    """[(state name, first boundary, last boundary)] from a --trace-state log (a change line per state transition)."""
    changes = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = STATE.search(line)
            if m:
                changes.append((int(m.group(1)), m.group(2)))
    spans = []
    for i, (b, name) in enumerate(changes):
        if spans and spans[-1][0] == name:
            continue
        end = next((cb for cb, cn in changes[i + 1:] if cn != name), None)
        spans.append((name, b, end))
    return spans


SUMMARY = re.compile(r"\[te\.gfx\] summary: presents (\d+) \(with draws (\d+)\), vblanks (\d+), StartFrame (\d+) \(clears (\d+)\), "
                     r"sprites (\d+), turned (\d+), strips (\d+), text quads (\d+), draws (\d+) \((?:merged \d+, submitted \d+, )?dropped (\d+)\), textures (\d+) \(failed (\d+)\)")
# (draw merging added "merged N, submitted N, " inside the draws parenthesis; both forms parse)


def parse_summary(path):
    last = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = SUMMARY.search(line)
            if m:
                last = m
    if not last:
        return None
    keys = ["presents", "presents_with_draws", "vblanks", "start_frames", "clears", "sprites", "turned", "strips",
            "text_quads", "draws", "dropped", "textures", "textures_failed"]
    return dict(zip(keys, (int(x) for x in last.groups())))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="a PPM frame, or a directory of frames (the newest is judged, or --frame N)")
    ap.add_argument("--clear", help="the clear colour R,G,B (its share is bounded by --max-top)")
    ap.add_argument("--min-luminance", type=float, default=8.0)
    ap.add_argument("--max-dark", type=float, default=0.98)
    ap.add_argument("--max-top", type=float, default=0.90)
    ap.add_argument("--dark", type=float, default=16.0, help="luminance below which a pixel is dark")
    ap.add_argument("--max-distinct", type=int, default=1 << 20)
    ap.add_argument("--summary-log", help="runner stderr with [te.gfx] summary lines (draws-per-frame floor)")
    ap.add_argument("--min-draws", type=int, default=1, help="sprites + turned + strips + text quads the run must have made")
    ap.add_argument("--frame", type=int, default=None, help="judge frame_N.ppm of the directory instead of the newest frame")
    ap.add_argument("--state-log", help="runner stderr of a --trace-state run: the game must have reached --require-state")
    ap.add_argument("--require-state", default="Title", help="GameController state name the run must reach (default Title)")
    ap.add_argument("--min-state-boundaries", type=int, default=50, help="boundaries the required state must hold for")
    a = ap.parse_args()

    frame = newest_frame(a.path, a.frame)
    if not frame:
        print(json.dumps({"verdict": "no-frame", "frame": a.path, "frame_number": a.frame}))
        return 2
    clear = tuple(int(x) for x in a.clear.split(",")) if a.clear else None
    w, h, px = read_ppm(frame)
    m = measure(w, h, px, a.dark, a.max_distinct, clear)
    reasons = []
    if m["luminance_mean"] < a.min_luminance:
        reasons.append(f"black: luminance_mean {m['luminance_mean']} < {a.min_luminance}")
    if m["dark_share"] > a.max_dark:
        reasons.append(f"black: dark_share {m['dark_share']} > {a.max_dark}")
    if m["top_colour_share"] > a.max_top:
        reasons.append(f"flat: top_colour_share {m['top_colour_share']} > {a.max_top} (colour {m['top_colour']})")
    if clear is not None and m["clear_colour_share"] > a.max_top:
        reasons.append(f"clear colour: share {m['clear_colour_share']} > {a.max_top}")
    if a.summary_log:
        s = parse_summary(a.summary_log)
        if s is None:
            reasons.append("summary: no [te.gfx] summary line in the log")
        else:
            m["summary"] = s
            made = s["sprites"] + s["turned"] + s["strips"] + s["text_quads"]
            if made < a.min_draws:
                reasons.append(f"draws: {made} 2D draws over the run < {a.min_draws}")
            if s["dropped"] > 0:
                reasons.append(f"draws: {s['dropped']} draws dropped by the backend")
            if s["textures_failed"] > 0:
                reasons.append(f"textures: {s['textures_failed']} failed to decode")
    if a.state_log:
        spans = state_spans(a.state_log)
        hit = [(b, e) for name, b, e in spans if name == a.require_state]
        m["states"] = [[name, b, e] for name, b, e in spans]
        if not hit:
            reasons.append(f"state: {a.require_state} never reached ({', '.join(n for n, _, _ in spans) or 'no state lines'})")
        else:
            b, e = hit[0]
            held = (e if e is not None else b + a.min_state_boundaries) - b
            m["required_state"] = {"name": a.require_state, "first_boundary": b, "left_at": e, "held": held}
            if held < a.min_state_boundaries:
                reasons.append(f"state: {a.require_state} held {held} boundaries < {a.min_state_boundaries}")
    m["frame"] = frame
    m["verdict"] = "green" if not reasons else "red"
    m["reasons"] = reasons
    print(json.dumps(m))
    return 0 if not reasons else 1


if __name__ == "__main__":
    sys.exit(main())
