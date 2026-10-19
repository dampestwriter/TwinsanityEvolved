#!/usr/bin/env python3
"""scene_frame_judge.py -- the scene judge, the arena pattern per level: REGION LUMINANCE + CLEAR-COLOUR SHARE +
DRAWS-PER-FRAME FLOOR on one presented frame of a dump run (TE_GFX_DUMP_DIR / TE_GFX_DUMP_EVERY / TE_GFX_DUMP_AT) with the
runner's stderr beside it.

A level preset names the frame regions a correct frame of that place lights (fractions of the frame: x0,y0,x1,y1) with the
luminance band each must fall in, the clear colour whose share of the frame must stay under a bound (a frame that is mostly
its clear colour has no world), and the floor of the frame's own draw and triangle counts (the "[te.gfx] frame N -> path
(draws D, triangles T, scene rigid R placed P sky S shadows H)" line the runner prints when it dumps the frame -- the SCENE
must have drawn, not only the UI). Thresholds are FROZEN per preset (frozen: red on the broken binary,
green on the fix); --set overrides one for exploration and is reported as such.

  presets:
    title_world   the title screen's world (the opening scene's sea and sky, PAL frame ~550 of a dump-every-25 run): the
                  upper half is the sky (bright, warm), the lower half the sea (blue, mid); letterbox bars excluded
    hub           the hub (N. Sanity Isle beach after the opening): the sky band, the ground band, the scene's triangles
    jungle_entry  Jungle Bungle's entry: foliage (mid-dark, green-heavy), sky patches
    jungle_cave_mouth  the hubb cave mouth's tunnel (acceptable): mostly authored black, the lit tunnel ahead-left
    cryscave      Cavern Catastrophe's crystal cave (dim purple rock, the pen, the wooden track): drawn, not blacked out
  The hub thresholds are PROVISIONAL until a frame is captured; jungle_entry is frozen over the jungle log by
  tools/judges/scene_set_judge.py + fixtures/judges/jungle_bungle_scene_pal.json.

Usage: scene_frame_judge.py DIR --frame N --log run.log --preset title_world [--set key=value ...] [--json]
Exit 0 = green (one JSON line of the measures), 1 = red, 2 = no frame / no log line.
"""
import argparse
import collections
import json
import os
import re
import sys

PRESETS = {
    "title_world": {
        "regions": {
            # x0, y0, x1, y1 (fractions), min luminance, max luminance
            "sky": (0.05, 0.16, 0.95, 0.45, 60.0, 240.0),
            "sea": (0.05, 0.55, 0.95, 0.84, 20.0, 200.0),
        },
        "clear": None,            # the frame's clear colour (R,G,B) or None = the most common colour
        "max_clear_share": 0.35,  # the most common colour may not cover more than this share of the frame
        "min_draws": 20,          # the frame's draws (UI + scene)
        "min_triangles": 2000,    # the frame's triangles: the scene's meshes, not the UI's quads
        "min_scene": 1,           # rigid + placed + sky models drawn since the previous dump
        "min_colours": 2000,
        "blue_sea": True,         # the sea region must be blue-dominant (mean B > mean R)
    },
    "hub": {
        "regions": {
            "sky": (0.05, 0.12, 0.95, 0.35, 40.0, 245.0),
            "ground": (0.05, 0.55, 0.95, 0.90, 12.0, 220.0),
        },
        "clear": None,
        "max_clear_share": 0.35,
        "min_draws": 30,
        "min_triangles": 5000,
        "min_scene": 10,
        "min_colours": 3000,
        "blue_sea": False,
    },
    "jungle_entry": {
        "regions": {
            "canopy": (0.05, 0.10, 0.95, 0.50, 10.0, 200.0),
            "floor": (0.05, 0.55, 0.95, 0.90, 8.0, 200.0),
        },
        "clear": None,
        "max_clear_share": 0.35,
        "min_draws": 30,
        "min_triangles": 8000,
        "min_scene": 10,
        "min_colours": 3000,
        "blue_sea": False,
    },
    # acceptable, attributed: the hubb cave mouth of Jungle Bungle between the totem gate and the lit interior
    # (jungle_bungle_walk.padlog presents ~7190-7290): the chase camera travels through a tunnel whose walls the level paints
    # BLACK (model 0xd95360 submodel 1: 89 of 189 authored vertex colours under 16; the gate 0xd95344 the same), so most
    # of the frame is drawn black and the clear/dark share may reach 0.6. A correct frame still DRAWS the scene (the
    # draw/triangle/model floors) and shows the lit tunnel ahead on the left (the "tunnel" region).
    # Cavern Catastrophe's cryscave (the cryscave camera, 2026-10-17): a dark purple cave -- the pen with the totem gate,
    # the wooden track, the crystal spiral. A correct frame is dim (upper / lower region means 20-45 on the fixed build) but DRAWN:
    # the mis-culled particle quads took the upper/lower regions to 0.5-10 with the most-common colour (black) over 0.24-0.89 of the frame.
    "cryscave": {
        "regions": {
            "upper": (0.05, 0.10, 0.95, 0.50, 12.0, 200.0),
            "lower": (0.05, 0.55, 0.95, 0.90, 12.0, 200.0),
        },
        "clear": None,
        "max_clear_share": 0.20,
        "min_draws": 30,
        "min_triangles": 8000,
        "min_scene": 10,
        "min_colours": 1500,      # a close rock wall fills the frame with few colours (present 4345: 2230); the dips are judged by the regions
        "blue_sea": False,
    },
    "jungle_cave_mouth": {
        "regions": {
            "tunnel": (0.02, 0.02, 0.45, 0.55, 8.0, 200.0),
        },
        "clear": None,
        "max_clear_share": 0.70,
        "min_draws": 30,
        "min_triangles": 8000,
        "min_scene": 10,
        "min_colours": 3000,
        "blue_sea": False,
    },
}


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
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
    px = data[pos:pos + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError(f"{path}: truncated")
    return w, h, px


def region_stats(w, h, px, box):
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    n = 0
    lum = 0.0
    r_sum = g_sum = b_sum = 0
    for y in range(y0, y1):
        row = (y * w) * 3
        for x in range(x0, x1):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            lum += 0.299 * r + 0.587 * g + 0.114 * b
            r_sum += r
            g_sum += g
            b_sum += b
            n += 1
    if n == 0:
        return {"luminance": 0.0, "r": 0.0, "g": 0.0, "b": 0.0}
    return {"luminance": lum / n, "r": r_sum / n, "g": g_sum / n, "b": b_sum / n}


def frame_log_line(log_path, frame):
    pattern = re.compile(r"\[te\.gfx\] frame (\d+) -> \S+ \(draws (\d+), triangles (\d+), scene rigid (\d+) placed (\d+) sky (\d+) shadows (\d+)\)")
    with open(log_path, errors="replace") as f:
        for line in f:
            m = pattern.search(line)
            if m and int(m.group(1)) == frame:
                return {"draws": int(m.group(2)), "triangles": int(m.group(3)), "rigid": int(m.group(4)), "placed": int(m.group(5)),
                        "sky": int(m.group(6)), "shadows": int(m.group(7))}
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--frame", type=int, required=True)
    ap.add_argument("--log", required=True, help="the runner's stderr of the dump run")
    ap.add_argument("--preset", choices=sorted(PRESETS), required=True)
    ap.add_argument("--set", action="append", default=[], help="override a threshold: key=value (reported; exploration only)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    preset = json.loads(json.dumps(PRESETS[a.preset]))
    overrides = {}
    for kv in a.set:
        k, v = kv.split("=", 1)
        preset[k] = json.loads(v)
        overrides[k] = preset[k]
    path = os.path.join(a.dir, f"frame_{a.frame:06d}.ppm")
    if not os.path.exists(path):
        print(f"RED: no frame {path}", file=sys.stderr)
        return 2
    counts = frame_log_line(a.log, a.frame)
    if counts is None:
        print(f"RED: no '[te.gfx] frame {a.frame} -> ...' line in {a.log}", file=sys.stderr)
        return 2
    w, h, px = read_ppm(path)
    colours = collections.Counter()
    for i in range(0, len(px), 3):
        colours[px[i:i + 3]] += 1
    top_colour, top_count = colours.most_common(1)[0]
    clear = tuple(preset["clear"]) if preset["clear"] else tuple(top_colour)
    clear_share = colours.get(bytes(clear), 0) / (w * h)
    result = {"preset": a.preset, "frame": a.frame, "size": [w, h], "clear": list(clear), "clear_share": round(clear_share, 4),
              "distinct_colours": len(colours), "counts": counts, "regions": {}, "overrides": overrides}
    reasons = []
    for name, (x0, y0, x1, y1, lo, hi) in preset["regions"].items():
        st = region_stats(w, h, px, (x0, y0, x1, y1))
        result["regions"][name] = {k: round(v, 2) for k, v in st.items()}
        if not (lo <= st["luminance"] <= hi):
            reasons.append(f"region {name} luminance {st['luminance']:.1f} outside [{lo}, {hi}]")
        if name == "sea" and preset.get("blue_sea") and not (st["b"] > st["r"]):
            reasons.append(f"region sea not blue-dominant (R {st['r']:.0f} B {st['b']:.0f})")
    if clear_share > preset["max_clear_share"]:
        reasons.append(f"clear colour share {clear_share:.3f} > {preset['max_clear_share']}")
    if len(colours) < preset["min_colours"]:
        reasons.append(f"distinct colours {len(colours)} < {preset['min_colours']}")
    if counts["draws"] < preset["min_draws"]:
        reasons.append(f"draws {counts['draws']} < {preset['min_draws']}")
    if counts["triangles"] < preset["min_triangles"]:
        reasons.append(f"triangles {counts['triangles']} < {preset['min_triangles']}")
    scene = counts["rigid"] + counts["placed"] + counts["sky"]
    if scene < preset["min_scene"]:
        reasons.append(f"scene models {scene} < {preset['min_scene']}")
    result["verdict"] = "GREEN" if not reasons else "RED"
    result["reasons"] = reasons
    print(json.dumps(result))
    if reasons:
        for r in reasons:
            print(f"RED: {r}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
