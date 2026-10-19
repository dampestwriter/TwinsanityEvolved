#!/usr/bin/env python3
"""speedrun_hud_judge.py -- SPEEDRUN MODE draws ONE thing: an RTA timer (mm:ss.cc) centred at the top of the safe area.

Two deterministic headless runs of the hub pad log (fixtures/padlogs/hub_entry.padlog: NEW GAME starts the run) with the
text-draw census on (TE_GFX_DIAG_TEXT=1: every Font::Draw logs its string, its quads' box in frame pixels and the frame size):
  A  without the mod                      -> the frame's own text draws (the HUD, the hints) at the judged presents.
  B  `--mod speedrun --record-pad <rec>`  -> the SPEEDRUN STRINGS at a judged present = the strings B draws that A does not
                                             (matched by the quad hash: the glyph sequence); GREEN needs, at EVERY judged present,
                                             EXACTLY ONE such string, reading mm:ss.cc (digits, a colon, a point: no label, no IGT
                                             line, no note), its box's centre x within 2 % of the frame's centre and its box inside
                                             the top band; no platform text of the run says rta / igt / speedrun / split; the timer
                                             READS DIFFERENTLY between the judged presents (it runs) and the run started at NEW GAME.
                                             (The splits in the recording and the practice ring stay speedrun_judge.py's business:
                                             the hub log never leaves its chunk, so it writes no split.)
A runner refusing `--mod` (the pre-mods play binary, exit 2) is RED. A binary whose census carries no strings (the census without strings:
only the first quad's corner) is judged on what it logs: the B-only draws' estimated boxes and the `platform texts` line's text.

RED on bin/twinsanityevolved.310b6f0 (two lines 'rta M:SS.cc' / 'igt M:SS.cc' bottom-left), GREEN on the single-timer build.

Usage: speedrun_hud_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "speedrun_hud_pal.json")

# the census line: the form with strings carries box / frame / string, the older form stops at the quad hash
TEXT_RE = re.compile(r"present (\d+) text (\d+) quads .* first \(([-\d.]+) ([-\d.]+)\)-\(([-\d.]+) ([-\d.]+)\) glyph ([-\d.]+)x([-\d.]+) quadhash ([0-9a-f]+)"
                     r"(?: box \(([-\d.]+) ([-\d.]+)\)-\(([-\d.]+) ([-\d.]+)\) frame (\d+)x(\d+) at \(([-\d.]+) ([-\d.]+)\) align (\S+) string '([^']*)')?")
PLATFORM_RE = re.compile(r"(?:present (\d+) )?platform texts (\d+): (.*)$", re.M)
TIMER_RE = re.compile(r"^\d{2,}:\d{2}\.\d{2}$")
HUD_WORDS = re.compile(r"rta|igt|speedrun|split", re.I)


def run(binary, iso, out, fx, extra, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_NO_DRAW"] = "1"
    env["TE_GFX_DIAG_TEXT"] = "1"
    print("speedrun_hud_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("speedrun_hud_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc, log


def census(text):
    """present -> list of draws {hash, box (x0 y0 x1 y1), frame (w h), string or None}"""
    out = {}
    for m in TEXT_RE.finditer(text):
        present, count = int(m.group(1)), int(m.group(2))
        fx0, fy0, fx1, fy1, gw, gh = (float(m.group(i)) for i in range(3, 9))
        d = {"hash": m.group(9), "count": count}
        if m.group(10) is not None:
            d["box"] = tuple(float(m.group(i)) for i in range(10, 14))
            d["frame"] = (int(m.group(14)), int(m.group(15)))
            d["string"] = m.group(19)
        else:
            # the older census: the first quad's corner and glyph size -- the box estimated as one line of `count` glyphs
            d["box"] = (fx0, fy0, fx0 + gw * count, fy0 + gh)
            d["frame"] = None
            d["string"] = None
        out.setdefault(present, []).append(d)
    return out


def platform_lines(text):
    """present (or None) -> the list of platform strings logged"""
    out = []
    for m in PLATFORM_RE.finditer(text):
        present = int(m.group(1)) if m.group(1) else None
        strings = re.findall(r"'([^']*)'", m.group(3))
        out.append((present, strings))
    return out


def speedrun_strings(a_draws, b_draws):
    """B's draws not in A (by quad hash, as a multiset), grouped by string (a string on two font pages = two draws)"""
    pool = {}
    for d in a_draws:
        pool[d["hash"]] = pool.get(d["hash"], 0) + 1
    only = []
    for d in b_draws:
        if pool.get(d["hash"], 0) > 0:
            pool[d["hash"]] -= 1
        else:
            only.append(d)
    groups = []
    for d in only:
        key = d["string"]
        g = next((g for g in groups if key is not None and g["string"] == key), None)
        if g is None:
            groups.append({"string": key, "box": list(d["box"]), "frame": d["frame"], "draws": 1})
        else:
            g["box"] = [min(g["box"][0], d["box"][0]), min(g["box"][1], d["box"][1]), max(g["box"][2], d["box"][2]), max(g["box"][3], d["box"][3])]
            g["draws"] += 1
    return groups


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "speedrun_hud_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    da, rca, log_a = run(a.bin, a.iso, a.out, fx, [], "A_no_mod")
    if rca == 2:
        print("speedrun_hud_judge: RED -- the runner refuses the run (no mod framework in this build)")
        return 1
    rec = os.path.join(a.out, "B_speedrun.padlog")
    db, rcb, log_b = run(a.bin, a.iso, a.out, fx, ["--mod", "speedrun", "--record-pad", rec], "B_speedrun")
    if rcb == 2:
        print("speedrun_hud_judge: RED -- the runner refuses --mod speedrun (no speedrun mode in this build)")
        return 1
    if rca != 0 or rcb != 0:
        print("speedrun_hud_judge: a run failed (A exit %d, B exit %d)" % (rca, rcb))
        return 2
    with open(log_a, errors="replace") as f:
        text_a = f.read()
    with open(log_b, errors="replace") as f:
        text_b = f.read()
    ca, cb = census(text_a), census(text_b)
    plat = platform_lines(text_b)
    frame_w, frame_h = fx.get("frame", [512, 512])
    centre_tol, top_band = fx["centre_tolerance"], fx["top_band"]
    reasons, timers = [], []
    for present in fx["presents"]:
        groups = speedrun_strings(ca.get(present, []), cb.get(present, []))
        # the platform texts the frame carried (the census with strings keys them by present; the older one does not)
        lines = [s for p, s in plat if p == present] or ([s for p, s in plat if p is None][-1:] if any(p is None for p, _ in plat) else [])
        strings = lines[0] if lines else []
        desc = ", ".join("'%s' %s box (%.0f %.0f)-(%.0f %.0f)" % (g["string"] if g["string"] is not None else "?", "x%d" % g["draws"] if g["draws"] > 1 else "",
                                                                 *g["box"]) for g in groups) or "none"
        print("speedrun_hud_judge: present %d: speedrun strings %d [%s]; platform texts %s" % (present, len(groups), desc, strings))
        if len(groups) != 1:
            if groups:
                ub = [min(g["box"][0] for g in groups), min(g["box"][1] for g in groups), max(g["box"][2] for g in groups), max(g["box"][3] for g in groups)]
                w, h = groups[0]["frame"] or (frame_w, frame_h)
                print("speedrun_hud_judge: present %d: the %d speedrun draws together span (%.0f %.0f)-(%.0f %.0f): centre (%.3f, %.3f) of the %dx%d frame"
                      % (present, len(groups), ub[0], ub[1], ub[2], ub[3], (ub[0] + ub[2]) / 2.0 / w, (ub[1] + ub[3]) / 2.0 / h, w, h))
            reasons.append("present %d: %d speedrun strings, not one" % (present, len(groups)))
            continue
        g = groups[0]
        w, h = g["frame"] or (frame_w, frame_h)
        cx = (g["box"][0] + g["box"][2]) / 2.0 / w
        cy = (g["box"][1] + g["box"][3]) / 2.0 / h
        s = g["string"] if g["string"] is not None else (strings[0] if strings else "")
        if g["draws"] > 1 and g["string"] is None:
            reasons.append("present %d: the one string is %d draws in a census without strings" % (present, g["draws"]))
        print("speedrun_hud_judge: present %d: string '%s' centre (%.3f, %.3f) of the %dx%d frame" % (present, s, cx, cy, w, h))
        timers.append(s)
        if not TIMER_RE.match(s):
            reasons.append("present %d: '%s' is not a bare mm:ss.cc timer" % (present, s))
        if abs(cx - 0.5) > centre_tol:
            reasons.append("present %d: centre x %.3f is off the frame's centre by more than %.0f %%" % (present, cx, centre_tol * 100))
        if g["box"][3] / h > top_band:
            reasons.append("present %d: the box reaches %.3f of the frame, below the top band (%.2f)" % (present, g["box"][3] / h, top_band))
        if len(strings) != 1:
            reasons.append("present %d: %d platform texts, not one" % (present, len(strings)))
    hud = sorted({s for _, strings in plat for s in strings if HUD_WORDS.search(s)})
    if hud:
        reasons.append("other speedrun HUD strings drawn: %d distinct, e.g. %s" % (len(hud), hud[:3]))
    if len(set(timers)) < 2 and len(fx["presents"]) >= 2:
        reasons.append("the timer does not run (reads %s at every judged present)" % sorted(set(timers)))
    recorded = os.path.isfile(rec)
    if not recorded:
        reasons.append("no recording written (the pad log is where the splits go)")
    started = "speedrun: run started" in text_b
    if not started:
        reasons.append("the run never started")
    print("speedrun_hud_judge: timers %s; run started %s; recording written %s; platform texts with HUD words: %d%s"
          % (timers, started, recorded, len(hud), (" e.g. %s" % hud[:3]) if hud else ""))
    if not reasons:
        print("speedrun_hud_judge: GREEN -- one mm:ss.cc timer, centred at the top; nothing else of the speedrun HUD on screen")
        return 0
    for r in reasons:
        print("speedrun_hud_judge: RED -- " + r)
    return 1


if __name__ == "__main__":
    sys.exit(main())
