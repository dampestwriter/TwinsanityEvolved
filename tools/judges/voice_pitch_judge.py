#!/usr/bin/env python3
"""voice_pitch_judge.py -- the VOICE PITCH judge: speech lines play at their own sample rate.

    python3 tools/judges/voice_pitch_judge.py --binary BIN --iso PAL.iso --out /abs/OUT [--fixture fixtures/judges/audio_pitch_cavern_pal.json]
                                              [--hub-card build/padlog_smoke/mc_hub] [--reuse]

The run: the fixture's pad log in PLAY mode (a window, the audio device open, the pacer at 60 Hz) on a memory card warped to
the fixture's chunk (tools/warp_save.py over the hub card the padlog_smoke rows write), with the device-side audio tap
(tools/audio_tap: the SDL callback's output recorded to a WAV -- what the speakers get, on any build) and TE_STREAM_TRACE=1
(one line per music / speech window the game streams: `[audio] music window file N +0xOFF SIZE bytes`).

The reference: for every speech window the run streamed (a window of a `Crash6\\<language>.mb` bank), the clip's own
MSVp/VAGp header names its sample rate (big-endian at +16; the lines are 32000 / 48000 / 18000 Hz while the bank's .mh
table says 32000 for all of them), and STREAM.IRX keys the voice on at THAT rate (module code 0x4c14: pitch = rate *
0x1000 / 48000, written over the pitch the EE requested). The reference mix is therefore the clip decoded from the disc at
its header's rate -- what the PS2 (and PCSX2) play. No PCSX2 capture was provided under int_work/.../ref/, and the recomp
oracle's HLE keys the voice on at the EE's pitch (games/.../hle/audio/music_library.cpp Start -> StartMusicVoice with the
reserved pitch) -- the same defect, so the oracle is NOT a reference for this item (its retained-IOP arm is not compiled
into any frozen oracle binary). The judge prints this provenance every run.

The measure: the fundamental (f0: YIN-style autocorrelation on 40 ms windows every 10 ms, tools/judges/audio_pitch.py) is
tracked on the host mix around the window's boundary and on the reference; the clip is located by laying the reference's
voiced f0 contour over the host's at every offset and candidate speed (0.5 .. 1.6: played at `speed` x its rate a clip is
shorter and higher by that factor) and taking the best contour match; the MEDIAN per-hop f0 ratio host / reference over the
hops voiced in both, at that alignment, is the clip's ratio. GREEN iff every speech clip the fixture
expects was found, every clip's median f0 ratio is within the fixture's tolerance (±3 %) of 1 and the speed ratio agrees,
and the audio census says dropped=0 underruns=0 (the standing audio dropout gate). Exit 0 GREEN, 1 RED, 2 on a run failure.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import audio_pitch as ap  # noqa: E402
import warp_save  # noqa: E402


# ----- a minimal ISO 9660 reader (the disc is read at run time only; nothing of it is kept) -------------------------
class Iso9660:
    SECTOR = 2048

    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self._sector(16)
        if pvd[1:6] != b"CD001":
            raise SystemExit("%s: not an ISO 9660 image" % path)
        root = pvd[156:156 + 34]
        self.root_extent, self.root_size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]

    def _sector(self, n, count=1):
        self.f.seek(n * self.SECTOR)
        return self.f.read(count * self.SECTOR)

    def _entries(self, extent, size):
        data = self._sector(extent, (size + self.SECTOR - 1) // self.SECTOR)[:size]
        pos = 0
        while pos < len(data):
            length = data[pos]
            if length == 0:
                pos = (pos // self.SECTOR + 1) * self.SECTOR
                continue
            rec = data[pos:pos + length]
            ext, sz, flags, nlen = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0], rec[25], rec[32]
            name = rec[33:33 + nlen]
            if name not in (b"\x00", b"\x01"):
                yield name.decode("ascii", "replace").split(";")[0].upper(), ext, sz, bool(flags & 2)
            pos += length

    def find(self, path):
        """path like 'Crash6\\English.mb' (case-insensitive, / or \\). Returns (extent, size) or None."""
        parts = [p for p in re.split(r"[\\/]", path) if p]
        extent, size = self.root_extent, self.root_size
        for i, part in enumerate(parts):
            hit = None
            for name, ext, sz, isdir in self._entries(extent, size):
                if name == part.upper():
                    hit = (ext, sz, isdir)
                    break
            if hit is None:
                return None
            extent, size, isdir = hit
            if i + 1 < len(parts) and not isdir:
                return None
        return extent, size

    def read(self, path, offset, length):
        found = self.find(path)
        if not found:
            return None
        extent, size = found
        if offset >= size:
            return b""
        self.f.seek(extent * self.SECTOR + offset)
        return self.f.read(min(length, size - offset))


# ----- the run -----------------------------------------------------------------------------------------------------
def build_tap():
    lib = os.path.join(ROOT, "tools", "audio_tap", "libsdl_audio_tap.dylib" if sys.platform == "darwin" else "libsdl_audio_tap.so")
    if not os.path.exists(lib) or os.path.getmtime(lib) < os.path.getmtime(os.path.join(ROOT, "tools", "audio_tap", "sdl_audio_tap.c")):
        subprocess.check_call([os.path.join(ROOT, "tools", "audio_tap", "build.sh")])
    return lib


def run(binary, iso, fx, out, hub_card, tap):
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    shutil.copytree(hub_card, mc)
    bank = None
    for root, _, files in os.walk(mc):
        if "Bank0.bin" in files:
            bank = os.path.join(root, "Bank0.bin")
    if not bank:
        raise SystemExit("voice_pitch_judge: no Bank0.bin under the hub card %s" % hub_card)
    with open(bank, "rb") as f:
        data = f.read()
    with open(bank, "wb") as f:
        f.write(warp_save.warp(data, fx["chunk"], int(fx.get("place", "0xFFFF"), 0)))
    wav = os.path.join(out, "mix.wav")
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--play-pad", os.path.join(ROOT, fx["padlog"]), "--skip-movies", "--trace-state",
            "--stop-at", str(fx["stop_at"]), "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 240)), "--no-sdl-input"]
    args += fx.get("extra_args", [])
    env = dict(os.environ)
    env["TE_STREAM_TRACE"] = "1"
    env["SDL_AUDIO_TAP"] = wav
    env["DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"] = tap
    env.pop("TE_GFX_HEADLESS", None)   # PLAY mode: a window and the device
    with open(log, "w") as f:
        f.write("# " + " ".join(args) + "\n")
        f.flush()
        rc = subprocess.call(args, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    return rc, log, wav


# ----- the measure -------------------------------------------------------------------------------------------------
def resample(x, rate_in, rate_out):
    t = np.arange(0, len(x) / rate_in, 1.0 / rate_out)
    return np.interp(t * rate_in, np.arange(len(x)), x)


def locate(host, hr, ref, rr, around_s, window_s, speeds, tol=1.03):
    """Finds the clip in the host mix near around_s by its f0 CONTOUR: the reference's voiced f0 track (10 ms hops) is
    laid over the host's at every offset and candidate speed (played at `speed` x its rate the clip is shorter and higher
    by that factor); the score is the share of the template's voiced hops whose host f0 is within `tol` of the template's.
    Speech contours are distinctive enough to survive the music under them. Returns (speed, start_s, score)."""
    lo = max(0.0, around_s - window_s)
    hi = min(len(host) / hr, around_s + window_s + len(ref) / rr * 2.0)
    th = ap.f0_track(host[int(lo * hr):int(hi * hr)], hr)
    hv = th[:, 1] > 0
    hlog = np.where(hv, np.log2(np.maximum(th[:, 1], 1.0)), 0.0)
    tr = ap.f0_track(resample(ref, rr, hr), hr)
    rv = tr[:, 1] > 0
    rlog = np.where(rv, np.log2(np.maximum(tr[:, 1], 1.0)), 0.0)
    best = (0.0, 1.0, lo)
    for speed in speeds:
        n = int(len(tr) / speed)
        if n < 10 or len(hlog) < n:
            continue
        idx = np.minimum((np.arange(n) * speed).astype(int), len(tr) - 1)
        tv, tlog = rv[idx], rlog[idx] + np.log2(speed)
        H = np.lib.stride_tricks.sliding_window_view(hlog, n)
        HV = np.lib.stride_tricks.sliding_window_view(hv, n)
        match = ((np.abs(H - tlog) < np.log2(tol)) & HV & tv).sum(axis=1) / max(int(tv.sum()), 1)
        k = int(np.argmax(match))
        if match[k] > best[0]:
            best = (float(match[k]), float(speed), lo + k * 0.01)
    return best[1], best[2], best[0]


def f0_ratio(host, hr, ref, rr, start_s, speed):
    """Median per-hop f0 ratio host / reference over hops voiced in both, the reference aligned at `speed`."""
    ref48 = resample(ref, rr * speed, hr)          # the reference as the host plays it in time (its f0 x speed)
    seg = host[int(start_s * hr):int(start_s * hr) + len(ref48)]
    th, tr = ap.f0_track(seg, hr), ap.f0_track(ref48, hr)
    n = min(len(th), len(tr))
    both = (th[:n, 1] > 0) & (tr[:n, 1] > 0)
    if both.sum() < 20:
        return None, int(both.sum())
    # tr's f0 is the reference stretched to the host's speed: its pitch is speed x the true one; undo it
    ratio = th[:n, 1][both] / (tr[:n, 1][both] / speed)
    return float(np.median(ratio)), int(both.sum())


def main():
    apr = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    apr.add_argument("--binary", required=True)
    apr.add_argument("--iso", required=True)
    apr.add_argument("--out", required=True)
    apr.add_argument("--fixture", default=os.path.join(ROOT, "fixtures", "judges", "audio_pitch_cavern_pal.json"))
    apr.add_argument("--hub-card", default=os.path.join(ROOT, "build", "padlog_smoke", "mc_hub"),
                     help="the hub card (mc0/BESLES-52568) the warp derives from")
    apr.add_argument("--reuse", action="store_true", help="judge the run already in --out")
    a = apr.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    print("voice_pitch_judge: reference = each streamed speech clip decoded from the disc at its MSVp/VAGp header's sample rate "
          "(STREAM.IRX 0x4c14's key-on rule); no PCSX2 capture under int_work/.../ref/; the recomp oracle's HLE shares the defect "
          "and is not used.")
    if a.reuse:
        rc, log, wav = 0, os.path.join(a.out, "run.log"), os.path.join(a.out, "mix.wav")
    else:
        tap = build_tap()
        rc, log, wav = run(a.binary, a.iso, fx, a.out, a.hub_card, tap)
    problems = []
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    if not any("--stop-at %d reached" % fx["stop_at"] in l for l in lines):
        problems.append("the run did not reach --stop-at %d (exit %d)" % (fx["stop_at"], rc))
    files = {}
    windows = []
    boundary = 0
    for l in lines:
        m = re.search(r"\[stream\] open #(\d+) \"([^\"]+)\"", l)
        if m:
            files[int(m.group(1))] = m.group(2)
        m = re.search(r"game state @ boundary (\d+)", l) or re.search(r"\[probe\] @(\d+)", l)
        if m:
            boundary = int(m.group(1))
        m = re.search(r"\[audio\] music window file (\d+) \+0x([0-9A-Fa-f]+) (\d+) bytes read", l)
        if m:
            windows.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3)), boundary))
    census = next((l for l in lines if "audio: device=" in l), "")
    m = re.search(r"dropped=(\d+) underruns=(\d+)", census)
    if not m:
        problems.append("no audio census line")
    elif m.group(1) != "0" or m.group(2) != "0":
        problems.append("audio census: dropped=%s underruns=%s" % (m.group(1), m.group(2)))
    speech = [(files.get(fnum, "?"), off, size, b) for fnum, off, size, b in windows
              if re.search(r"\.mb$", files.get(fnum, ""), re.I) and not re.search(r"music\.mb$", files.get(fnum, ""), re.I)
              and b >= fx.get("from_boundary", 0)]
    print("speech windows in the run: %d (%s)" % (len(speech), ", ".join("%s +0x%X @%d" % (os.path.basename(p.replace("\\", "/")), o, b) for p, o, s, b in speech)))
    if len(speech) < fx.get("min_speech_clips", 1):
        problems.append("fewer speech clips than the fixture expects (%d < %d)" % (len(speech), fx.get("min_speech_clips", 1)))
    results = []
    if os.path.exists(wav) and speech:
        host, hr = ap.read_wav(wav)
        iso = Iso9660(a.iso)
        ratios = np.arange(fx.get("speed_min", 0.5), fx.get("speed_max", 1.6) + 1e-9, 0.01)
        for path, off, size, b in speech[:fx.get("max_speech_clips", 4)]:
            raw = iso.read(path, off, size)
            if not raw or raw[:4] not in (b"MSVp", b"VAGp"):
                problems.append("%s +0x%X: no VAGp/MSVp header on the disc (cannot build the reference)" % (path, off))
                continue
            rate = struct.unpack_from(">I", raw, 16)[0]
            name = raw[32:48].rstrip(b"\0").decode("ascii", "replace")
            ref = ap.decode_vag(raw[48:])
            if rate < 4000 or rate > 96000 or len(ref) < rate // 2:
                problems.append("%s +0x%X: header rate %d / %d samples (not a speech clip)" % (path, off, rate, len(ref)))
                continue
            around = b / 60.0 + fx.get("device_lead_s", 0.05)
            speed, start, score = locate(host, hr, ref, rate, around, fx.get("search_window_s", 6.0), ratios)
            ratio, hops = f0_ratio(host, hr, ref, rate, start, speed)
            results.append((name, rate, speed, start, score, ratio, hops))
            print("  %s (+0x%X, %d Hz, %.2f s): found at %.2f s, speed ratio %.2f (contour match %.2f), median f0 ratio host/reference %s over %d voiced hops"
                  % (name, off, rate, len(ref) / rate, start, speed, score, "n/a" if ratio is None else "%.4f" % ratio, hops))
            tol = fx.get("tolerance", 0.03)
            if score < fx.get("min_match", 0.2):
                problems.append("%s: the clip was not located in the mix (contour match %.2f < %.2f)" % (name, score, fx.get("min_match", 0.2)))
            if ratio is None:
                problems.append("%s: too few voiced hops to measure (%d)" % (name, hops))
            elif abs(ratio - 1.0) > tol:
                problems.append("%s: median f0 ratio %.4f outside 1 +/- %.2f" % (name, ratio, tol))
            if abs(speed - 1.0) > fx.get("speed_tolerance", 0.05):
                problems.append("%s: plays at %.2f x its own rate" % (name, speed))
    elif speech:
        problems.append("no mix.wav (the audio tap did not record: is the device open?)")
    with open(os.path.join(a.out, "judge.json"), "w") as f:
        json.dump({"fixture": a.fixture, "binary": a.binary, "clips": [dict(zip(("name", "rate", "speed", "start_s", "match", "f0_ratio", "hops"), r)) for r in results],
                   "problems": problems, "census": census.strip()}, f, indent=1)
    verdict = "GREEN" if not problems else "RED"
    print("voice_pitch_judge: %s%s" % (verdict, "" if not problems else " -- " + "; ".join(problems)))
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
