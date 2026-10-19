#!/usr/bin/env python3
"""scene_set_judge.py -- the frozen scene judge of ONE pad log: every dumped Playing frame of a deterministic headless run
judged by scene_frame_judge's preset, with the fixture's DOCUMENTED WINDOWS (a present range, the preset that holds there, the
cause) and, per window, EVIDENCE that the dark region is drawn, authored-dark geometry (the runner's TE_GFX_DIAG_PRESENT line
of one present inside the window: a draw of the given kind and clip mode whose submodel has at least `min_dark_share` of
its authored vertex colours under 16, whose emitted vertices reach `min_box_right` and whose mean colour is under
`max_mean_lum`). fixtures/judges/jungle_bungle_scene_pal.json.

Usage: scene_set_judge.py --bin BIN --iso IMAGE [--fixture F] [--out DIR]        run + judge
       scene_set_judge.py --dir DUMPDIR --log run.log [--fixture F]                judge an existing dump
Exit 0 = GREEN (every frame + every window's evidence), 1 = RED, 2 = the run failed / nothing to judge.
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
sys.path.insert(0, HERE)
import scene_frame_judge  # noqa: E402

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "jungle_bungle_scene_pal.json")
FRAME = re.compile(r"\[te\.gfx\] frame (\d+) -> \S+ \(draws (\d+), triangles (\d+), scene rigid (\d+) placed (\d+) sky (\d+) shadows (\d+)\)")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")
DIAG = re.compile(r"\[te\.gfx\.diag\] present (\d+) (\w+) model (0x[0-9a-f]+) part (\d+)/(\d+) (\w+) bucket (\d+) .* tris (\d+) box \((-?\d+) (-?\d+)\)-\((-?\d+) (-?\d+)\) rgba \((\d+) (\d+) (\d+) (\d+)\) n (\d+)")
COLOURS = re.compile(r"\[te\.gfx\.diag\]   colours of (\d+) vertices: dark\(<16\) (\d+) mid\(<64\) (\d+) bright (\d+)")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    dump = os.path.join(out, "dump")
    for d in (mc, dump):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    log = os.path.join(out, "run.log")
    if fx.get("card", "fresh").startswith("warp:"):
        # the authoring warp (tools/warp_save.py): the hub card written by hub_entry.padlog, its save's chunk rewritten
        hub = os.path.join(out, "mc_hub")
        shutil.rmtree(hub, ignore_errors=True)
        os.makedirs(hub)
        env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
        subprocess.call([binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
                         "--play-pad", os.path.join(ROOT, "fixtures/padlogs/hub_entry.padlog"), "--mc-dir", hub, "--stop-at", "2700"],
                        stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "hub_card.log"), "w"), env=env, cwd=ROOT)
        shutil.rmtree(mc, ignore_errors=True)
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools/warp_save.py"), "--card", hub, "--chunk", fx["card"][5:],
                               "--out", mc, "--place", "0xFFFF"], stdout=subprocess.DEVNULL, cwd=ROOT)
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    evidence = [w["evidence_present"] for w in fx.get("windows", []) if "evidence_present" in w]
    if evidence:
        env["TE_GFX_DIAG_PRESENT"] = str(evidence[0])   # one present's draws logged (the first window's)
    print("scene_set_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("scene_set_judge: exit", rc, "->", log, flush=True)
    return dump, log, rc


def frame_states(log_path):
    """present -> the GameController state in force when the frame was presented (log order)"""
    states = {}
    state = "?"
    with open(log_path, errors="replace") as f:
        for line in f:
            m = STATE.search(line)
            if m:
                state = m.group(2)
                continue
            m = FRAME.search(line)
            if m:
                states[int(m.group(1))] = state
    return states


def window_evidence(log_path, window):
    """The diag lines of the window's evidence present: a draw matching the window's evidence rule"""
    want = window["evidence"]
    present = window["evidence_present"]
    found = []
    with open(log_path, errors="replace") as f:
        lines = f.read().splitlines()
    for i, line in enumerate(lines):
        m = DIAG.search(line)
        if not m or int(m.group(1)) != present:
            continue
        kind, clip = m.group(2), m.group(6)
        tris, x1 = int(m.group(8)), int(m.group(11))
        r, g, b = int(m.group(13)), int(m.group(14)), int(m.group(15))
        dark_share = None
        if i + 1 < len(lines):
            c = COLOURS.search(lines[i + 1])
            if c and int(c.group(1)) > 0:
                dark_share = int(c.group(2)) / int(c.group(1))
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        if kind != want["kind"] or (clip == "clipped") != want["clipped"] or tris == 0:
            continue
        if dark_share is None or dark_share < want["min_dark_share"]:
            continue
        if x1 < want["min_box_right"] or lum > want["max_mean_lum"]:
            continue
        found.append({"model": m.group(3), "part": m.group(4), "tris": tris, "box_right": x1, "mean_lum": round(lum, 1),
                      "dark_share": round(dark_share, 3)})
    return found


def judge(dump, log_path, fx):
    states = frame_states(log_path)
    frames = sorted(int(f[6:12]) for f in os.listdir(dump) if f.startswith("frame_") and f.endswith(".ppm"))
    frames = [n for n in frames if fx["frames_from"] <= n <= fx["frames_to"]]
    if not frames:
        print("scene_set_judge: RED -- no frames in range", file=sys.stderr)
        return 2
    skip = set(fx.get("skip_states", []))
    red = []
    judged = 0
    skipped = []
    windows = fx.get("windows", [])
    for n in frames:
        state = states.get(n, "?")
        if state in skip:
            skipped.append((n, state))
            continue
        preset = fx["preset"]
        window = None
        for w in windows:
            if w["from"] <= n <= w["to"]:
                preset, window = w["preset"], w
        argv = [dump, "--frame", str(n), "--log", log_path, "--preset", preset]
        sys.argv = ["scene_frame_judge.py"] + argv
        import io
        import contextlib
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = scene_frame_judge.main()
        judged += 1
        verdict = out.getvalue().strip().splitlines()[-1] if out.getvalue().strip() else ""
        tag = (" [window: %s]" % window["name"]) if window else ""
        print("%6d %-12s %-18s %s%s" % (n, state, preset, "GREEN" if rc == 0 else "RED", tag))
        if rc != 0:
            red.append((n, preset, err.getvalue().strip().replace("\n", "; "), verdict))
    for w in windows:
        if "evidence" not in w:
            continue
        found = window_evidence(log_path, w)
        if found:
            print("window '%s': evidence at present %d -- %s" % (w["name"], w["evidence_present"], json.dumps(found[0])))
        else:
            red.append((w["evidence_present"], w["preset"], "window '%s': no draw of authored-dark geometry covering the dark region logged at present %d (TE_GFX_DIAG_PRESENT)" % (w["name"], w["evidence_present"]), ""))
    print("scene_set_judge: %d frames judged (%d skipped: %s), %d red" % (judged, len(skipped), ", ".join(sorted(set(s for _, s in skipped))) or "-", len(red)))
    for n, preset, why, verdict in red:
        print("RED: present %d (%s): %s" % (n, preset, why), file=sys.stderr)
    print("scene_set_judge: %s" % ("GREEN" if not red else "RED"))
    return 0 if not red else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--dir", help="an existing dump directory (with --log)")
    ap.add_argument("--log")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the run's directory (default: a temp dir under the build)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    if a.dir:
        if not a.log:
            ap.error("--dir needs --log")
        return judge(a.dir, a.log, fx)
    if not a.bin or not a.iso:
        ap.error("--bin and --iso (or --dir and --log)")
    out = a.out or os.path.join(ROOT, "build", "scene_set_judge")
    dump, log, rc = run(a.bin, a.iso, out, fx)
    if rc != 0:
        print("scene_set_judge: RED -- the run exited %d" % rc, file=sys.stderr)
        return 2
    return judge(dump, log, fx)


if __name__ == "__main__":
    sys.exit(main())
