#!/usr/bin/env python3
"""padlog_judge.py -- the frozen judge of a pad-log replay : the run's log must show the log replayed to its
end and the game's heap intact.

    padlog_judge.py RUN_LOG --fixture fixtures/judges/<name>.json

The fixture freezes: the states the trace must visit in order (`states`, GameController names), the state the run must
end in (`final_state`), the boundary the run must reach (`stop_at`, the `--stop-at N reached` line), and the heap floor
(`min_free_pages`: the small allocations' pages left at exit -- a per-frame leak exhausts them, the game then gets NULL
from its allocator and traps in whatever reads the NULL arrays). Red on any `wasm trap`, exit non-zero.

Frozen on the frame-memory leak fix (platform/nativeee/wasm/graphics_resources.cpp AllocFrameMemory/SubmitBuckets): RED on
main 2d31e23 (trap at boundary 1702, pages 0 free), GREEN on the fix (4200 boundaries, Playing held, pages free > 0).
"""
import argparse
import json
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--fixture", required=True)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    text = open(a.log, errors="replace").read()
    problems = []
    measures = {}

    traps = re.findall(r"wasm trap: (.*)", text)
    measures["traps"] = traps
    if traps:
        problems.append(f"trap: {traps[0]}")

    states = re.findall(r"game state @ boundary (\d+): (\w+) \((\d+)\)", text)
    trace = [(int(b), name) for b, name, _ in states]
    measures["transitions"] = len(trace)
    pos = 0
    for want in fx["states"]:
        while pos < len(trace) and trace[pos][1] != want:
            pos += 1
        if pos >= len(trace):
            problems.append(f"state {want} not reached (after the ones before it)")
            break
        measures[f"state_{want}"] = trace[pos][0]
        pos += 1
    if trace:
        measures["final_state"] = trace[-1][1]
        if fx.get("final_state") and trace[-1][1] != fx["final_state"]:
            problems.append(f"final state {trace[-1][1]} (wanted {fx['final_state']})")
    else:
        problems.append("no state trace (run with --trace-state)")

    m = re.search(r"--stop-at (\d+) reached", text)
    measures["stop_at_reached"] = int(m.group(1)) if m else None
    if not m or int(m.group(1)) < fx["stop_at"]:
        problems.append(f"the run did not reach --stop-at {fx['stop_at']}")

    m = re.search(r"clock: (\d+) boundaries", text)
    measures["boundaries"] = int(m.group(1)) if m else None

    m = re.search(r"heap: pool (\d+) bytes(?: at 0x[0-9A-Fa-f]+)?, used (\d+), peak (\d+), free (\d+); small pages used (\d+) of (\d+) \(free (\d+)\)", text)
    if m:
        measures["heap_peak"] = int(m.group(3))
        measures["pages_used"], measures["pages"], measures["pages_free"] = int(m.group(5)), int(m.group(6)), int(m.group(7))
        if measures["pages_free"] < fx["min_free_pages"]:
            problems.append(f"small pages free {measures['pages_free']} < {fx['min_free_pages']} (a frame leak?)")
    else:
        problems.append("no heap census line (the runner prints it at exit)")

    verdict = "GREEN" if not problems else "RED"
    print(f"padlog_judge: {verdict} {a.fixture}")
    for k, v in measures.items():
        print(f"  {k}: {v}")
    for p in problems:
        print(f"  RED: {p}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
