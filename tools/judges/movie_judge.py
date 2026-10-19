#!/usr/bin/env python3
"""movie_judge.py -- the movie judge: the boot movies PLAY through the game's Player (decoded, presented, drawn, heard).

Reads the runner's stderr (the audio/stream/movie census the runner prints at exit, platform/nativeee/host/
audio_service.cpp Report) and judges the movie line:

  [twinsanityevolved] movie: constructs=C opens=O failed=F skipped=S closes=X unknown=U; pictures decoded=D queued=Q
      uploaded=P presents=R draws=W fields=L bytes=B; pcm frames=A silence=Z[ (skip movies)]

green when: opens >= --min-opens (default 2: the Vivendi and Travellers Tales logos of a boot), failed == 0, skipped == 0,
closes == opens (every movie ran its lifecycle to Close), decoded == queued == uploaded >= --min-pictures (default 400: the two
logos are 470 pictures at 25 fps), presents >= min-pictures, draws >= presents - 2 * opens (the first picture of each
movie is not drawn: the PS2 draws from the second), pcm frames >= --min-pcm (default 100000: ~2 s of 48 kHz) with
silence == 0 (the decoder never ran behind the mixer's tick), and the line is not marked "(skip movies)". The stub's
line ("(stub)", no picture counts) is red: the regex does not match.

With a frame (--frame PPM, or --dir DIR --frame-number N: a TE_GFX_DUMP_AT frame of the deterministic boot) the presented
picture is judged too, with title_frame_judge's measures: luminance_mean >= --min-luminance (default 150: the Vivendi
logo's white field; a black frame is 0) and top_colour_share <= --max-top (default 0.97: the logo over the white).

The frozen fixture: fixtures/judges/movie_boot_pal.json (thresholds + the frame number of the deterministic PAL boot; the
judge takes --fixture FILE and reads them). Red on the stub binary and on --skip-movies; green on the fix.

Exit 0 = green (one JSON line of the measures), 1 = red (including no census line: the stub binary), 2 = no frame.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from title_frame_judge import measure, read_ppm  # noqa: E402

MOVIE = re.compile(r"\[twinsanityevolved\] movie: constructs=(\d+) opens=(\d+) failed=(\d+) skipped=(\d+) closes=(\d+) unknown=(\d+); "
                   r"pictures decoded=(\d+) queued=(\d+) uploaded=(\d+) presents=(\d+) draws=(\d+) fields=(\d+) bytes=(\d+); "
                   r"pcm frames=(\d+) silence=(\d+)(.*)$")
KEYS = ["constructs", "opens", "failed", "skipped", "closes", "unknown", "decoded", "queued", "uploaded", "presents", "draws",
        "fields", "bytes", "pcm_frames", "pcm_silence"]


def parse_movie_line(path):
    last = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = MOVIE.search(line)
            if m:
                last = m
    if not last:
        return None
    out = dict(zip(KEYS, (int(x) for x in last.groups()[:15])))
    out["skip_movies"] = "skip movies" in last.group(16)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="the runner's stderr (the census at exit)")
    ap.add_argument("--fixture", help="JSON of thresholds (fixtures/judges/movie_boot_pal.json); arguments given override it")
    ap.add_argument("--frame", help="a presented frame (PPM) during a movie")
    ap.add_argument("--dir", help="a TE_GFX_DUMP_DIR directory; with --frame-number N judges frame_N.ppm")
    ap.add_argument("--frame-number", type=int, default=None)
    ap.add_argument("--min-opens", type=int, default=None)
    ap.add_argument("--min-pictures", type=int, default=None)
    ap.add_argument("--min-pcm", type=int, default=None)
    ap.add_argument("--min-luminance", type=float, default=None)
    ap.add_argument("--max-top", type=float, default=None)
    a = ap.parse_args()

    th = {"min_opens": 2, "min_pictures": 400, "min_pcm": 100000, "min_luminance": 150.0, "max_top": 0.97, "frame_number": None}
    if a.fixture:
        with open(a.fixture) as f:
            th.update(json.load(f).get("thresholds", {}))
    for k in ("min_opens", "min_pictures", "min_pcm", "min_luminance", "max_top", "frame_number"):
        v = getattr(a, k)
        if v is not None:
            th[k] = v

    m = parse_movie_line(a.log)
    if m is None:
        print(json.dumps({"verdict": "red", "log": a.log, "reasons": ["no movie census line (the stub's line, or no run)"]}))
        return 1
    reasons = []
    if m["skip_movies"]:
        reasons.append("skip: the run skipped every movie (--skip-movies)")
    if m["opens"] < th["min_opens"]:
        reasons.append(f"opens: {m['opens']} < {th['min_opens']}")
    if m["failed"] != 0:
        reasons.append(f"failed: {m['failed']} movies could not be opened")
    if m["skipped"] != 0:
        reasons.append(f"skipped: {m['skipped']} movies ended at their first Step")
    if m["closes"] != m["opens"]:
        reasons.append(f"lifecycle: closes {m['closes']} != opens {m['opens']}")
    if not (m["decoded"] == m["queued"] == m["uploaded"]):
        reasons.append(f"pictures: decoded {m['decoded']} queued {m['queued']} uploaded {m['uploaded']} differ")
    if m["decoded"] < th["min_pictures"]:
        reasons.append(f"pictures: {m['decoded']} decoded < {th['min_pictures']}")
    if m["presents"] < th["min_pictures"]:
        reasons.append(f"presents: {m['presents']} < {th['min_pictures']}")
    if m["draws"] < m["presents"] - 2 * m["opens"]:
        reasons.append(f"draws: {m['draws']} < presents {m['presents']} - 2 per movie")
    if m["pcm_frames"] < th["min_pcm"]:
        reasons.append(f"sound: {m['pcm_frames']} PCM frames popped < {th['min_pcm']}")
    if m["pcm_silence"] != 0:
        reasons.append(f"sound: {m['pcm_silence']} frames of silence where the track should have been")
    frame = a.frame
    if a.dir and th["frame_number"] is not None:
        frame = os.path.join(a.dir, f"frame_{int(th['frame_number']):06d}.ppm")
    if frame:
        if not os.path.exists(frame):
            print(json.dumps({"verdict": "no-frame", "frame": frame, "census": m}))
            return 2
        w, h, px = read_ppm(frame)
        f = measure(w, h, px, 16.0, 1 << 20, None)
        m["frame"] = frame
        m["frame_measures"] = f
        if f["luminance_mean"] < th["min_luminance"]:
            reasons.append(f"frame: luminance_mean {f['luminance_mean']} < {th['min_luminance']} (no picture drawn)")
        if f["top_colour_share"] > th["max_top"]:
            reasons.append(f"frame: top_colour_share {f['top_colour_share']} > {th['max_top']} (a flat frame, no picture)")
    m["thresholds"] = th
    m["verdict"] = "green" if not reasons else "red"
    m["reasons"] = reasons
    print(json.dumps(m))
    return 0 if not reasons else 1


if __name__ == "__main__":
    sys.exit(main())
