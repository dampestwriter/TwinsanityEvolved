#!/usr/bin/env python3
"""frame_hash_equality_judge.py -- are two builds' PRESENTED FRAMES identical on the fixture's pad logs?

A renderer refactor that must not move a pixel (the typed-draw surface decoupled from the GL device behind
render::Backend) is gated by this judge: each build replays the fixture's pad logs deterministically with the picture
drawn and the harness leg armed (`--harness-out`: frames.fhash, one `<key>,0x<fnv1a64>` per presented frame, the hash of
the presented RGB), and the two legs' frames.fhash must be IDENTICAL -- the same number of presents, the same hash on
every line, for every pad log. GREEN before and GREEN after BY DESIGN; a RED means the refactor changed the picture.

  frame_hash_equality_judge.py run --bin BIN --iso ISO --out DIR [--fixture F] [--only NAME,...] [--no-draw]
      replays every pad log of the fixture with BIN into DIR/<name>/ (frames.fhash, run.log, the card)
  frame_hash_equality_judge.py compare DIR_A DIR_B [--fixture F] [--only NAME,...]
      compares the legs: per pad log the present count, the first / last hash, the sha256 of frames.fhash

Exit 0 = GREEN (identical on every pad log), 1 = RED, 2 = a run failed / a leg is missing.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "frame_hash_equality_pal.json")


def load_fixture(path, only):
    with open(path) as f:
        fx = json.load(f)
    rows = fx["padlogs"]
    if only:
        wanted = set(only.split(","))
        rows = [r for r in rows if r["name"] in wanted]
        missing = wanted - {r["name"] for r in rows}
        if missing:
            print("frame_hash_equality_judge: unknown pad log(s):", ", ".join(sorted(missing)))
            sys.exit(2)
    return fx, rows


def prepare_card(row, out, hub_card):
    card = os.path.join(out, "mc")
    shutil.rmtree(card, ignore_errors=True)
    spec = row.get("card", "fresh")
    if spec.startswith("warp:"):
        if not hub_card or not os.path.isdir(hub_card):
            print("frame_hash_equality_judge: %s needs the hub_entry card (run that pad log first)" % row["name"])
            sys.exit(2)
        rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", hub_card,
                              "--chunk", spec[len("warp:"):], "--out", card, "--place", "0xFFFF"],
                             stdout=subprocess.DEVNULL, cwd=ROOT)
        if rc != 0:
            print("frame_hash_equality_judge: warp_save.py failed for", row["name"])
            sys.exit(2)
    else:
        os.makedirs(card)
    return card


def run_one(binary, iso, row, out, hub_card, no_draw):
    os.makedirs(out, exist_ok=True)
    card = prepare_card(row, out, hub_card)
    leg = os.path.join(out, "leg")
    shutil.rmtree(leg, ignore_errors=True)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, row["padlog"]), "--mc-dir", card, "--stop-at", str(row["stop_at"]),
            "--run-seconds", str(row.get("run_seconds", 600)), "--harness-out", leg]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env.pop("TE_GFX_NO_DRAW", None)
    if no_draw:
        env["TE_GFX_NO_DRAW"] = "1"   # the RED demonstration: a leg whose presents carry no picture
    print("frame_hash_equality_judge: %s: running %s" % (row["name"], " ".join(args)), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    fhash = os.path.join(leg, "frames.fhash")
    reached = False
    with open(log, errors="replace") as f:
        for line in f:
            if "--stop-at %d reached" % row["stop_at"] in line:
                reached = True
    n = count_lines(fhash) if os.path.isfile(fhash) else -1
    print("frame_hash_equality_judge: %s: exit %d, stop-at reached %s, presents hashed %d -> %s" %
          (row["name"], rc, reached, n, fhash), flush=True)
    if rc != 0 or not reached or n <= 0:
        return False, card
    return True, card


def count_lines(path):
    with open(path) as f:
        return sum(1 for line in f if line.strip())


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def read_hashes(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(line)
    return rows


def cmd_run(a):
    fx, rows = load_fixture(a.fixture, a.only)
    ok = True
    hub_card = None
    # the hub's card first: the warp rows derive theirs from it
    rows = sorted(rows, key=lambda r: 0 if r["name"] == "hub_entry" else 1)
    for row in rows:
        good, card = run_one(a.bin, a.iso, row, os.path.join(a.out, row["name"]), hub_card, a.no_draw)
        if row["name"] == "hub_entry":
            hub_card = card
        ok = ok and good
    print("frame_hash_equality_judge: run", "OK" if ok else "FAILED", "->", a.out)
    return 0 if ok else 2


def cmd_compare(a):
    fx, rows = load_fixture(a.fixture, a.only)
    verdict = True
    for row in rows:
        name = row["name"]
        pa = os.path.join(a.dir_a, name, "leg", "frames.fhash")
        pb = os.path.join(a.dir_b, name, "leg", "frames.fhash")
        for p in (pa, pb):
            if not os.path.isfile(p):
                print("%s: MISSING %s" % (name, p))
                return 2
        ha, hb = read_hashes(pa), read_hashes(pb)
        same = ha == hb
        first_diff = next((i for i, (x, y) in enumerate(zip(ha, hb)) if x != y), None)
        print("%s: A %d presents [%s .. %s] sha256 %s" % (name, len(ha), ha[0] if ha else "-", ha[-1] if ha else "-", digest(pa)[:16]))
        print("%s: B %d presents [%s .. %s] sha256 %s" % (name, len(hb), hb[0] if hb else "-", hb[-1] if hb else "-", digest(pb)[:16]))
        if same:
            print("%s: IDENTICAL (%d presents)" % (name, len(ha)))
        else:
            differing = sum(1 for x, y in zip(ha, hb) if x != y) + abs(len(ha) - len(hb))
            where = "first difference at line %d: %s vs %s" % (first_diff + 1, ha[first_diff], hb[first_diff]) if first_diff is not None else "present counts differ"
            print("%s: DIFFERENT (%d lines differ; %s)" % (name, differing, where))
        verdict = verdict and same
    print("frame_hash_equality_judge:", "GREEN (every pad log's presented frames identical)" if verdict else "RED (a presented frame differs)")
    return 0 if verdict else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--bin", required=True)
    r.add_argument("--iso", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--fixture", default=DEFAULT_FIXTURE)
    r.add_argument("--only", default="")
    r.add_argument("--no-draw", action="store_true", help="the RED demonstration leg: TE_GFX_NO_DRAW=1")
    c = sub.add_parser("compare")
    c.add_argument("dir_a")
    c.add_argument("dir_b")
    c.add_argument("--fixture", default=DEFAULT_FIXTURE)
    c.add_argument("--only", default="")
    a = ap.parse_args()
    sys.exit(cmd_run(a) if a.cmd == "run" else cmd_compare(a))


if __name__ == "__main__":
    main()
