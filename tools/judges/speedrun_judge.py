#!/usr/bin/env python3
"""speedrun_judge.py -- SPEEDRUN MODE: does it draw its timer with the game's font (one mm:ss.cc line, top centre), write its
splits into the pad-log recording, and save / load practice states on its own ring? (The timer's placement and content are
the speedrun_hud_judge's business; this judge covers the splits, the practice ring and that the timer is drawn at all.)

Three deterministic headless runs of the level-select pad log (the title's menus -> warp to level -> cavern catastrophe:
fixtures/padlogs/level_select_toggle_row3.padlog) with `--mod level_select`:
  A  without the speedrun mod                      -> the reference frame (dumped every 100 presents).
  B  `--mod speedrun --record-pad <rec>`           -> GREEN needs: the recording holds a `# split enter <chunk>` line with rta= / igt=
                                                      fields for the warped level (a split on the level entry), the run log shows
                                                      the timer text crossing the game's text system (`platform texts ... 'mm:ss.cc'`),
                                                      and on the judged present the timer's region (top centre) differs from A's
                                                      (mean |A-B| >= `region_min_diff`, B's luminance SD >= A's + `region_min_sd_gain`)
                                                      while the rows below it are identical (<= `rest_max_diff`).
  C  `--mod speedrun` + F9 at `save_at`, F10 at `load_at` (the harness's --inject-key; the keys re-fire after every rewind, so the
     run loops until its watchdog)                 -> GREEN needs: a practice state saved to slot 0, loaded IN PLACE at least once,
                                                      and the probe's boundaries rewinding (a `@<save_at+..>` line after a `@<load_at-..>`).
A runner refusing `--mod` (a play binary without mods, exit 2) is RED.

RED on bin/twinsanityevolved.0f92911, GREEN on the speedrun-mod build.

Usage: speedrun_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "speedrun_pal.json")


def run(binary, iso, out, fx, extra, tag, env_extra=None, run_seconds=None, stop_at=None):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    dump, mc = os.path.join(d, "dump"), os.path.join(d, "mc")
    os.makedirs(dump)
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(stop_at or fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(run_seconds or fx.get("run_seconds", 600)), "--mod", "level_select"] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    env["TE_GFX_DIAG_TEXT"] = "1"
    if env_extra:
        env.update(env_extra)
    print("speedrun_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("speedrun_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc, log


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
    w, h, px = read_ppm(path)
    x0, y0, x1, y1 = (int(round(region[0] * w)), int(round(region[1] * h)), int(round(region[2] * w)), int(round(region[3] * h)))
    out = []
    for y in range(y0, y1):
        row = px[y * w * 3:(y + 1) * w * 3]
        for x in range(x0, x1):
            i = x * 3
            out.append(0.299 * row[i] + 0.587 * row[i + 1] + 0.114 * row[i + 2])
    return out


def mad(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i] - b[i]) for i in range(n)) / n if n else 0.0


def sd(v):
    if not v:
        return 0.0
    m = sum(v) / len(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / len(v))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "speedrun_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    da, rca, _ = run(a.bin, a.iso, a.out, fx, [], "A_no_speedrun")
    if rca == 2:
        print("speedrun_judge: RED -- the runner refuses --mod (no mod framework, no speedrun mode)")
        return 1
    rec = os.path.join(a.out, "B_speedrun.padlog")
    db, rcb, log_b = run(a.bin, a.iso, a.out, fx, ["--mod", "speedrun", "--record-pad", rec], "B_speedrun")
    if rca != 0 or rcb != 0:
        print("speedrun_judge: a run failed (A exit %d, B exit %d)" % (rca, rcb))
        return 2
    # the splits in the recording
    with open(rec, errors="replace") as f:
        rec_text = f.read()
    split_re = re.compile(r"^# split enter %s rta=([0-9.]+) igt=([0-9.]+) boundary=(\d+)" % re.escape(fx["chunk"]), re.M)
    split = split_re.search(rec_text)
    splits = len(re.findall(r"^# split ", rec_text, re.M))
    with open(log_b, errors="replace") as f:
        log_text = f.read()
    timer_lines = len(re.findall(r"platform texts \d+: '\d{2,}:\d{2}\.\d{2}'", log_text))
    # the timer in the frame
    present = fx["present"]
    fa, fb = os.path.join(da, "dump", "frame_%06d.ppm" % present), os.path.join(db, "dump", "frame_%06d.ppm" % present)
    if not (os.path.isfile(fa) and os.path.isfile(fb)):
        print("speedrun_judge: no frame %d in both runs" % present)
        return 2
    ra, rb = luminances(fa, fx["region"]), luminances(fb, fx["region"])
    region_diff, sd_a, sd_b = mad(ra, rb), sd(ra), sd(rb)
    rest_diff = mad(luminances(fa, fx["rest"]), luminances(fb, fx["rest"]))
    # the practice ring
    keys = ["--inject-key", "F9@%d-%d" % (fx["save_at"], fx["save_at"]), "--inject-key", "F10@%d-%d" % (fx["load_at"], fx["load_at"])]
    dc, rcc, log_c = run(a.bin, a.iso, a.out, fx, ["--mod", "speedrun", "--trace-player", "50"] + keys, "C_practice_ring",
                         env_extra={"TE_GFX_NO_DRAW": "1"}, run_seconds=fx["ring_seconds"])
    with open(log_c, errors="replace") as f:
        c_text = f.read()
    saved = "practice state -> slot 0" in c_text
    loads = c_text.count("loaded in place")
    probe_b = [int(m) for m in re.findall(r"^\[probe\] @(\d+) pos", c_text, re.M)]
    rewound = any(probe_b[i] > fx["load_at"] - 100 and probe_b[i + 1] < fx["save_at"] + 100 for i in range(len(probe_b) - 1))
    print("speedrun_judge: B: split on entry %s (%s), %d split lines, %d timer text lines; present %d region %s: A sd %.2f, B sd %.2f, "
          "|A-B| %.2f (need >= %.1f, sd gain >= %.1f), rest |A-B| %.3f (max %.1f). C: exit %d, slot 0 saved %s, loaded in place %d, "
          "boundaries rewound %s" % ("YES" if split else "NO", split.group(0)[2:] if split else "-", splits, timer_lines, present, fx["region"],
                                     sd_a, sd_b, region_diff, fx["region_min_diff"], fx["region_min_sd_gain"], rest_diff, fx["rest_max_diff"],
                                     rcc, saved, loads, rewound))
    green = (split is not None and timer_lines >= fx["min_timer_lines"] and region_diff >= fx["region_min_diff"] and
             sd_b - sd_a >= fx["region_min_sd_gain"] and rest_diff <= fx["rest_max_diff"] and rcc == 0 and saved and loads >= 1 and rewound)
    if green:
        print("speedrun_judge: GREEN -- the timer is drawn with the game's font, the level entry wrote a split into the recording, "
              "the practice ring saves and loads in place")
        return 0
    print("speedrun_judge: RED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
