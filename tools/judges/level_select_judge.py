#!/usr/bin/env python3
"""level_select_judge.py -- LEVEL SELECT, one click, always available: does Options > Mods > level select open the list at once
(no enable / warp toggle, no "set warp levels" step), does a chosen row warp through the game's own path, and is the mod on
(saves to the -mods card) from the moment the list opens?

MENU DEPTH. A fixture's depth = the menu activations (CROSS presses) after START that it takes to reach the level, plus one for
every mod the fixture must pre-enable with `--mod` (that flag stands for the switch the player would have to step first). The
one-click fixtures (fixtures/padlogs/level_select_row*.padlog: options, mods, level select, the row = 4, no --mod) must be
ONE SHALLOWER than the older fixture of the same row (fixtures/padlogs/level_select_toggle_row*.padlog: options, mods, warp to
level, the row = 4, plus `--mod level_select` = 5 -- without the flag its Mods page shows the toggle first and the walk misses).

For each sampled row of fixtures/judges/level_select_pal.json a deterministic headless run plays the one-click pad log WITHOUT
`--mod`, the authoring probe and the text-draw census on. GREEN for a row iff the log says the mod went ON before the row was
chosen (`mods: level_select ON` precedes `mods: level_select row N chosen -> action 1 warp to '<chunk>'`: opening the list is
what enables it), the saves moved to the -mods card, the probe then names the player's chunk as that chunk, GameController
reached Playing, no trap, no GameOver; and the MENU DUMP (the census strings of a frame showing the Mods page: the 'mods'
title and the mods' rows) has the level select row as the bare string 'level select' -- no value appended (no 'level select :
off' / ': on' toggle row; the speedrun mode row keeps its value). GREEN overall iff the depth is one fewer and every sampled row is GREEN.

A runner refusing the run (exit 2) is RED. On bin/twinsanityevolved.310b6f0 the one-click walk's third CROSS steps the toggle
instead of opening the list, the DOWNs land on other rows and no level is reached: RED. GREEN on the one-click level-select build.

Usage: level_select_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "level_select_pal.json")
TEXT_RE = re.compile(r"present (\d+) text \d+ quads .*? string '([^']*)'")


def run(binary, iso, out, fx, row, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--trace-player", "100", "--play-pad", os.path.join(ROOT, row["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_NO_DRAW"] = "1"
    env["TE_GFX_DIAG_TEXT"] = "1"
    print("level_select_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("level_select_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return rc, log


def depth(padlog, mods):
    """menu activations after START (CROSS = 0x4000 in the wire word) + the mods the fixture pre-enables"""
    presses, started = 0, False
    with open(os.path.join(ROOT, padlog)) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            word = int(line.split()[1], 16)
            if word & 0x0008:
                started = True
            elif started and word & 0x4000:
                presses += 1
    return presses + len(mods), presses


def menu_dump(text):
    """the strings of each present's text draws (the census with strings); {} when the census carries no strings"""
    out = {}
    for m in TEXT_RE.finditer(text):
        out.setdefault(int(m.group(1)), []).append(m.group(2))
    return out


def mods_page_rows(dump):
    """the rows of a frame showing the Mods page (the 'mods' title, the level select row, the speedrun mode row; the back row
    is the game's own localised text), or None"""
    for present in sorted(dump):
        strings = dump[present]
        if "mods" in strings and any(s.startswith("level select") for s in strings) and any(s.startswith("speedrun mode") for s in strings):
            return present, strings
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "level_select_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    green = True
    for row in fx["rows"]:
        new_depth, new_presses = depth(row["padlog"], fx.get("mods", []))
        old_depth, old_presses = depth(row["old_padlog"], fx.get("old_mods", []))
        shallower = new_depth == old_depth - 1
        print("level_select_judge: row %d depth: one-click fixture %d (%d activations + %d pre-enabled mods) vs the older fixture %d (%d + %d) -> %s"
              % (row["row"], new_depth, new_presses, len(fx.get("mods", [])), old_depth, old_presses, len(fx.get("old_mods", [])),
                 "one fewer" if shallower else "NOT one fewer"))
        green = green and shallower
    for row in fx["rows"]:
        tag = "row%d" % row["row"]
        rc, log = run(a.bin, a.iso, a.out, fx, row, tag)
        if rc == 2:
            print("level_select_judge: RED -- the runner refuses the run (no mods page exists in this build)")
            return 1
        with open(log, errors="replace") as f:
            text = f.read()
        chunk = row["chunk"]
        on_at = text.find("mods: level_select ON")
        chosen_line = "mods: level_select row %d chosen -> action 1 warp to '%s'" % (row["row"], chunk)
        chosen_at = text.find(chosen_line)
        chosen = chosen_at >= 0
        on_first = on_at >= 0 and (chosen_at < 0 or on_at < chosen_at)
        mods_card = "saves -> the -mods card" in text[on_at:] if on_at >= 0 else False
        reached = ("chunk 0x" in text) and (("'%s'" % chunk) in text)
        playing = "Playing (12) <-" in text
        traps = len([l for l in text.splitlines() if re.search(r"wasm trap|trap:|Invalid call_indirect|SIGBUS|SIGSEGV", l) and "0 traps" not in l])
        over = "GameOver" in text
        dump = menu_dump(text)
        present, rows = mods_page_rows(dump)
        if rows is None:
            toggle_free, dump_desc = False, ("no strings in the census" if not dump else "no frame shows the Mods page")
        else:
            ls = [s for s in rows if s.startswith("level select")]
            toggle_free = ls == ["level select"]
            dump_desc = "present %d rows %s" % (present, sorted({s for s in rows if s != "mods" and not s.startswith("^") and not s.endswith("\\")}))
        ok = rc == 0 and chosen and on_first and mods_card and reached and playing and traps == 0 and not over and toggle_free
        last = re.findall(r"game state @ boundary \d+: [A-Za-z]+ \(\d+\)", text)
        print("level_select_judge: row %d (%s): exit %d, mod on before the row %s, -mods card %s, chosen %s, player's chunk %s, Playing %s, traps %d, "
              "GameOver %s, menu dump toggle-free %s (%s), last [%s] -> %s"
              % (row["row"], row["name"], rc, on_first, mods_card, chosen, reached, playing, traps, over, toggle_free, dump_desc,
                 last[-1] if last else "-", "OK" if ok else "FAIL"))
        green = green and ok
    if green:
        print("level_select_judge: GREEN -- one click: the level select row opens the list (the mod on from then), a chosen row warps through the "
              "game's level-entry path, one menu step fewer than before, no toggle row")
        return 0
    print("level_select_judge: RED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
