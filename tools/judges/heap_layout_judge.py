#!/usr/bin/env python3
"""heap_layout_judge.py -- the game's heap layout is the FROZEN BASELINE's, whatever the build's C++ data size.

The game's replay depends on its heap addresses (uninitialised reads, pointer-ordered sets: a 16-byte shift of the pools'
start desyncs jungle_bungle_walk.padlog at boundary 5893 and the log fails to reach the arena). Before the heap layout was frozen the pools started at
the module's __heap_base, which moved with every wasm-side stage; now the C++'s data sits below the retail image
(tools/link_wasm.py), the module's data end IS the image's end, and the pools start at a fixed address
(platform/nativeee/wasm/memory.cpp FixedPoolStart). This judge replays the short walk log headless and checks the run's
own layout lines against the fixture (fixtures/judges/heap_layout.json):
  module line    heap base == data end == image_end          (the C++'s data is below the image)
  heap line      the heap pool's address, size and page count (the frozen baseline's: 0x3F6490, 7960912, 963)
  the log replays to its end in the state the fixture names (the layout is the one the pad logs were recorded on)
RED on bin/twinsanityevolved.1952747 (heap base 0x3F6480 = its data end: the C++'s data above the image, no pool address
in its heap line), GREEN on the frozen-layout build.
Usage: heap_layout_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
       heap_layout_judge.py --log run.log [--fixture F]
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "heap_layout.json")


def run(binary, iso, out, fx):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    os.makedirs(mc)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--mc-dir", mc, "--stop-at", str(fx["stop_at"]),
            "--run-seconds", str(fx.get("run_seconds", 600))]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_NO_DRAW="1")
    print("heap_layout_judge: running", " ".join(args), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("heap_layout_judge: exit", rc, "->", log, flush=True)
    return rc, log


def judge(text, fx):
    problems = []
    m = re.search(r"heap base 0x([0-9A-Fa-f]+), data end 0x([0-9A-Fa-f]+), retail image at 0x([0-9A-Fa-f]+)", text)
    measures = {}
    if not m:
        problems.append("no module line")
    else:
        heap_base, data_end, image = (int(m.group(i), 16) for i in (1, 2, 3))
        measures.update(heap_base=hex(heap_base), data_end=hex(data_end), image=hex(image))
        if image != fx["image_start"]:
            problems.append(f"retail image at {image:#x} (wanted {fx['image_start']:#x})")
        if heap_base != fx["image_end"] or data_end != fx["image_end"]:
            problems.append(f"heap base {heap_base:#x} / data end {data_end:#x}: the module's data must end at the retail image's end "
                            f"{fx['image_end']:#x} (the C++'s data below the image)")
    m = re.search(r"heap: pool (\d+) bytes at 0x([0-9A-Fa-f]+), used \d+, peak \d+, free \d+; small pages used \d+ of (\d+)", text)
    if not m:
        problems.append("no heap line with the pool's address (a runner before the frozen heap layout, or the heap never started)")
    else:
        size, start, pages = int(m.group(1)), int(m.group(2), 16), int(m.group(3))
        measures.update(pool_size=size, pool_start=hex(start), pages=pages)
        if start != fx["pool_start"]:
            problems.append(f"heap pool at {start:#x} (the frozen baseline's {fx['pool_start']:#x})")
        if size != fx["pool_size"]:
            problems.append(f"heap pool {size} bytes (the frozen baseline's {fx['pool_size']})")
        if pages != fx["pages"]:
            problems.append(f"{pages} small pages (the frozen baseline's {fx['pages']})")
    m = re.search(r"--stop-at (\d+) reached", text)
    if not m or int(m.group(1)) < fx["stop_at"]:
        problems.append(f"the run did not reach --stop-at {fx['stop_at']}")
    states = re.findall(r"game state @ boundary (\d+): (\w+) \(", text)
    measures["final_state"] = states[-1][1] if states else None
    if not states or states[-1][1] != fx["final_state"]:
        problems.append(f"final state {measures['final_state']} (wanted {fx['final_state']})")
    traps = re.findall(r"wasm trap: (.*)", text)
    if traps:
        problems.append(f"trap: {traps[0]}")
    return problems, measures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--iso")
    ap.add_argument("--log")
    ap.add_argument("--out", help="the run's directory (default: build/heap_layout_judge)")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    for k in ("image_start", "image_end", "pool_start"):
        fx[k] = int(fx[k], 0) if isinstance(fx[k], str) else fx[k]
    if a.log:
        log = a.log
    elif a.bin and a.iso:
        rc, log = run(a.bin, a.iso, a.out or os.path.join(ROOT, "build", "heap_layout_judge"), fx)
        if rc != 0:
            print("heap_layout_judge: RED -- the run exited %d" % rc, file=sys.stderr)
            return 2
    else:
        ap.error("--bin and --iso (or --log)")
    problems, measures = judge(open(log, errors="replace").read(), fx)
    print(json.dumps(measures))
    for p in problems:
        print("heap_layout_judge:  " + p)
    print("heap_layout_judge: %s" % ("GREEN" if not problems else "RED"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
