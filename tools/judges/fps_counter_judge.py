#!/usr/bin/env python3
"""fps_counter_judge.py -- does the FPS COUNTER draw through the GAME's text system, top-right, only when it is on?

Two deterministic headless runs of the title to the main menu (fixtures/judges/fps_counter_pal.json; frames dumped every 100
presents): A with `--fps-counter off`, B with `--fps-counter on`. The counter is two lines of the game's font ("N fps" over
"N hz", Platform::Graphics::QueuePlatformTexts -> the game's QueueText -> Font::Draw -> the page materials' glyph quads) in
the top-right corner. GREEN iff, on the judged present:
  - the top-right REGION differs between A and B (mean absolute luminance difference >= `region_min_diff`) and B's region
    carries more structure than A's (luminance standard deviation up by >= `region_min_sd_gain`: glyphs over the sky);
  - the REST of the frame (the rows below the region) is the same in A and B (mean abs diff <= `rest_max_diff`: the
    counter changes nothing else);
  - B's run log says the counter is on and A's region equals A's own frame with the counter absent (nothing is drawn
    there when it is off: the region's diff against the previous dump of the same run stays under `rest_max_diff`,
    i.e. the menu's top-right is static sky).
A runner without `--fps-counter` (a play binary without the fps counter refuses the option, exit 2) is RED: no toggle exists.

RED on bin/twinsanityevolved.0f92911, GREEN on the fps-counter build.

Usage: fps_counter_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "fps_counter_pal.json")


def run(binary, iso, out, fx, extra, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    dump, mc = os.path.join(d, "dump"), os.path.join(d, "mc")
    os.makedirs(dump)
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    print("fps_counter_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("fps_counter_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, memoryview(data)[pos:pos + 3 * w * h]


def luminances(path, region):
    """The luminance of every pixel in the region (fractions x0 y0 x1 y1), row-major."""
    w, h, px = read_ppm(path)
    x0, y0 = int(round(region[0] * w)), int(round(region[1] * h))
    x1, y1 = int(round(region[2] * w)), int(round(region[3] * h))
    out = []
    for y in range(y0, y1):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(x0, x1):
            i = x * 3
            out.append(0.299 * row[i] + 0.587 * row[i + 1] + 0.114 * row[i + 2])
    return out


def mean_abs_diff(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i] - b[i]) for i in range(n)) / n if n else 0.0


def sd(v):
    if not v:
        return 0.0
    m = sum(v) / len(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / len(v))


def frame(d, present):
    return os.path.join(d, "dump", "frame_%06d.ppm" % present)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "fps_counter_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    da, rca = run(a.bin, a.iso, a.out, fx, ["--fps-counter", "off"], "A_off")
    if rca == 2:
        print("fps_counter_judge: RED -- the runner refuses --fps-counter (no fps counter toggle exists in this build)")
        return 1
    db, rcb = run(a.bin, a.iso, a.out, fx, ["--fps-counter", "on"], "B_on")
    if rca != 0 or rcb != 0:
        print("fps_counter_judge: a run failed (A exit %d, B exit %d)" % (rca, rcb))
        return 2
    present = fx["present"]
    fa, fb = frame(da, present), frame(db, present)
    for p in (fa, fb):
        if not os.path.isfile(p):
            print("fps_counter_judge: no frame %s" % p)
            return 2
    region, rest = fx["region"], fx["rest"]
    ra, rb = luminances(fa, region), luminances(fb, region)
    region_diff = mean_abs_diff(ra, rb)
    sd_a, sd_b = sd(ra), sd(rb)
    rest_diff = mean_abs_diff(luminances(fa, rest), luminances(fb, rest))
    # the region is static with the counter off: the previous dump of run A shows the same sky there
    prev = frame(da, present - fx["dump_every"])
    static_diff = mean_abs_diff(luminances(prev, region), ra) if os.path.isfile(prev) else 0.0
    with open(os.path.join(db, "run.log")) as f:
        on_logged = "fps counter on" in f.read()
    print("fps_counter_judge: present %d region %s: A sd %.2f, B sd %.2f, mean |A-B| %.2f (need >= %.1f, sd gain >= %.1f); "
          "rest |A-B| %.3f (max %.1f); A region vs previous dump %.3f (max %.1f); B log says on: %s"
          % (present, region, sd_a, sd_b, region_diff, fx["region_min_diff"], fx["region_min_sd_gain"], rest_diff,
             fx["rest_max_diff"], static_diff, fx["rest_max_diff"], on_logged))
    green = (region_diff >= fx["region_min_diff"] and sd_b - sd_a >= fx["region_min_sd_gain"] and rest_diff <= fx["rest_max_diff"]
             and static_diff <= fx["rest_max_diff"] and on_logged)
    if green:
        print("fps_counter_judge: GREEN -- the fps counter's glyphs sit in the top-right region when on and nothing is drawn there when off")
        return 0
    print("fps_counter_judge: RED -- %s" % ("the region does not change with the toggle" if region_diff < fx["region_min_diff"] else
                                         "the rest of the frame changed" if rest_diff > fx["rest_max_diff"] else "see the figures"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
