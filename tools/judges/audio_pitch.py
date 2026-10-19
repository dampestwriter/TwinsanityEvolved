#!/usr/bin/env python3
"""audio_pitch.py -- the fundamental-pitch (f0) toolkit behind voice_pitch_judge.py: reads a WAV (PCM 16/32 or IEEE float),
decodes a PS2 ADPCM (VAG) asset at its stated rate, segments speech by energy and tracks f0 per 20 ms window by normalised
autocorrelation (a YIN-style difference function with parabolic refinement). numpy only.

    python3 tools/judges/audio_pitch.py track mix.wav [--from S --to S] [--fmin 70 --fmax 400]
    python3 tools/judges/audio_pitch.py compare host.wav ref.wav [--host-from S --host-to S --ref-from S --ref-to S]
    python3 tools/judges/audio_pitch.py vag English.mb --offset 0xB30000 --size 199360 --rate 32000 --out line.wav
"""
import argparse
import struct
import sys

import numpy as np

HOP_MS = 10.0
WINDOW_MS = 40.0   # analysis window: ≥ 2 periods at fmin 70 Hz (28.6 ms)


# ----- WAV ------------------------------------------------------------------------------------------------------
def read_wav(path):
    """Returns (mono float64 samples in [-1, 1], rate). Handles PCM 16/24/32 and IEEE float 32 (the device-side tap)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("%s: not a RIFF/WAVE file" % path)
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif cid == b"data":
            pcm = body if size != 0xFFFFFFFF - 44 and size <= len(body) else data[pos + 8:]
            if size == 0 or pos + 8 + size > len(data):   # an unpatched tail (the program was killed): take what is there
                pcm = data[pos + 8:]
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise SystemExit("%s: no fmt/data chunk" % path)
    tag, channels, rate, _, _, bits = fmt
    if tag == 3 or (tag == 0xFFFE and bits == 32 and False):
        a = np.frombuffer(pcm[:len(pcm) // 4 * 4], dtype="<f4").astype(np.float64)
    elif bits == 16:
        a = np.frombuffer(pcm[:len(pcm) // 2 * 2], dtype="<i2").astype(np.float64) / 32768.0
    elif bits == 32:
        a = np.frombuffer(pcm[:len(pcm) // 4 * 4], dtype="<i4").astype(np.float64) / 2147483648.0
    elif bits == 24:
        b = np.frombuffer(pcm[:len(pcm) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        a = ((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) ^ 0x800000) - 0x800000
        a = a.astype(np.float64) / 8388608.0
    else:
        raise SystemExit("%s: unsupported format tag %d / %d bits" % (path, tag, bits))
    if channels > 1:
        a = a[:len(a) // channels * channels].reshape(-1, channels).mean(axis=1)
    return a, rate


def write_wav(path, samples, rate):
    s = np.clip(np.asarray(samples, dtype=np.float64), -1.0, 1.0)
    pcm = (s * 32767.0).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm)


# ----- PS2 ADPCM (VAG) ---------------------------------------------------------------------------------------------
_VAG_F = ((0.0, 0.0), (60 / 64.0, 0.0), (115 / 64.0, -52 / 64.0), (98 / 64.0, -55 / 64.0), (122 / 64.0, -60 / 64.0))


def decode_vag(blocks, stop_at_end=True):
    """16-byte blocks -> float samples (28 per block). Stops at the first block flagged LoopEnd without Repeat (0x01) when
    stop_at_end (the SPU2 keys the voice off there); a 0x07 (loop start) / 0x03 (repeat) block plays on."""
    out = np.zeros(len(blocks) // 16 * 28)
    h1 = h2 = 0.0
    n = 0
    for b in range(len(blocks) // 16):
        blk = blocks[b * 16:b * 16 + 16]
        shift, pred = blk[0] & 0x0F, (blk[0] >> 4) & 0x0F
        flags = blk[1]
        if pred > 4:
            pred = 0
        f0, f1 = _VAG_F[pred]
        for i in range(14):
            byte = blk[2 + i]
            for nib in (byte & 0x0F, byte >> 4):
                s = (nib ^ 8) - 8   # signed 4-bit
                s = float(s << 12) / float(1 << shift) if shift < 13 else 0.0
                s = s + h1 * f0 + h2 * f1
                s = max(-32768.0, min(32767.0, s))
                out[n] = s / 32768.0
                h2, h1 = h1, s
                n += 1
        if stop_at_end and (flags & 0x01) and not (flags & 0x02):
            break
    return out[:n]


# ----- f0 ------------------------------------------------------------------------------------------------------------
def frame_signal(x, rate, hop_ms=HOP_MS, win_ms=WINDOW_MS):
    hop, win = int(rate * hop_ms / 1000), int(rate * win_ms / 1000)
    if len(x) < win:
        return np.zeros((0, win)), hop, win
    count = 1 + (len(x) - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(count)[:, None]
    return x[idx], hop, win


def f0_track(x, rate, fmin=70.0, fmax=400.0, energy_db=-40.0, clarity=0.6):
    """Per 10 ms hop: (time_s, f0_hz or 0, rms). f0 by the YIN cumulative-mean normalised difference over a 40 ms window
    (the Hann-weighted autocorrelation form), parabolic interpolation around the minimum, voiced when the normalised
    difference is below (1 - clarity) and the frame's RMS is above energy_db relative to the file's loudest frame."""
    frames, hop, win = frame_signal(x, rate)
    if len(frames) == 0:
        return np.zeros((0, 3))
    frames = frames - frames.mean(axis=1, keepdims=True)
    rms = np.sqrt((frames ** 2).mean(axis=1))
    peak = rms.max() if rms.size else 1.0
    floor = peak * (10.0 ** (energy_db / 20.0))
    tau_min, tau_max = int(rate / fmax), int(np.ceil(rate / fmin))
    tau_max = min(tau_max, win // 2 - 1)
    w = np.hanning(win)
    fw = frames * w
    # autocorrelation by FFT per frame
    nfft = 1 << int(np.ceil(np.log2(2 * win)))
    spec = np.fft.rfft(fw, nfft, axis=1)
    ac = np.fft.irfft(spec * np.conj(spec), nfft, axis=1)[:, :win]
    # difference function d(tau) = r(0) + r_tau(0) - 2 r(tau); the YIN form with the energy terms over the window
    energy = np.cumsum(fw ** 2, axis=1)
    r0 = energy[:, -1:]
    taus = np.arange(win)
    e_tail = r0 - np.concatenate([np.zeros((len(frames), 1)), energy[:, :-1]], axis=1)   # sum_{j>=tau} x_j^2
    d = r0 + e_tail - 2.0 * ac
    d[:, 0] = 0.0
    cmnd = np.ones_like(d)
    cums = np.cumsum(d[:, 1:], axis=1)
    cmnd[:, 1:] = d[:, 1:] * np.arange(1, win) / np.maximum(cums, 1e-12)
    out = np.zeros((len(frames), 3))
    out[:, 0] = np.arange(len(frames)) * hop / rate
    out[:, 2] = rms
    band = cmnd[:, tau_min:tau_max + 1]
    for i in range(len(frames)):
        if rms[i] < floor:
            continue
        row = band[i]
        # the first dip below the threshold (YIN's absolute threshold), else the global minimum
        below = np.where(row < (1.0 - clarity))[0]
        if below.size:
            j = below[0]
            while j + 1 < row.size and row[j + 1] < row[j]:
                j += 1
        else:
            j = int(np.argmin(row))
            if row[j] > (1.0 - clarity) * 1.5:
                continue
        tau = tau_min + j
        if 1 <= tau < win - 1:
            a, b, c = cmnd[i, tau - 1], cmnd[i, tau], cmnd[i, tau + 1]
            denom = a - 2 * b + c
            if denom != 0:
                tau = tau + 0.5 * (a - c) / denom
        if tau > 0:
            out[i, 1] = rate / tau
    return out


def speech_segments(track, min_voiced_s=0.3, gap_s=0.25):
    """Groups voiced hops into segments (start_s, end_s, median_f0) separated by gaps longer than gap_s; segments shorter
    than min_voiced_s of voiced hops are dropped."""
    segs = []
    start = last = None
    voiced = []
    for t, f0, _ in track:
        if f0 > 0:
            if start is None:
                start = t
            elif t - last > gap_s:
                segs.append((start, last, voiced))
                start, voiced = t, []
            last = t
            voiced.append(f0)
    if start is not None:
        segs.append((start, last, voiced))
    out = []
    for s, e, v in segs:
        if len(v) * HOP_MS / 1000.0 >= min_voiced_s:
            out.append((s, e, float(np.median(v)), len(v)))
    return out


def summarize(track, label):
    voiced = track[track[:, 1] > 0]
    if not voiced.size:
        print("%s: no voiced frames" % label)
        return None
    med = float(np.median(voiced[:, 1]))
    print("%s: %d voiced hops of %d, median f0 %.1f Hz, p25 %.1f, p75 %.1f" %
          (label, len(voiced), len(track), med, np.percentile(voiced[:, 1], 25), np.percentile(voiced[:, 1], 75)))
    return med


def slice_seconds(x, rate, start, end):
    a = 0 if start is None else int(max(0.0, start) * rate)
    b = len(x) if end is None else int(end * rate)
    return x[a:b]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("track"); t.add_argument("wav"); t.add_argument("--from", dest="start", type=float); t.add_argument("--to", type=float)
    t.add_argument("--fmin", type=float, default=70.0); t.add_argument("--fmax", type=float, default=400.0); t.add_argument("--segments", action="store_true")
    c = sub.add_parser("compare"); c.add_argument("host"); c.add_argument("ref")
    for p in ("host", "ref"):
        c.add_argument("--%s-from" % p, type=float); c.add_argument("--%s-to" % p, type=float)
    c.add_argument("--fmin", type=float, default=70.0); c.add_argument("--fmax", type=float, default=400.0)
    v = sub.add_parser("vag"); v.add_argument("file"); v.add_argument("--offset", type=lambda s: int(s, 0), required=True)
    v.add_argument("--size", type=lambda s: int(s, 0), required=True); v.add_argument("--rate", type=int, default=32000); v.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "vag":
        with open(a.file, "rb") as f:
            f.seek(a.offset)
            blocks = f.read(a.size)
        pcm = decode_vag(blocks)
        write_wav(a.out, pcm, a.rate)
        print("decoded %d samples (%.2f s at %d Hz) -> %s" % (len(pcm), len(pcm) / a.rate, a.rate, a.out))
        return 0
    if a.cmd == "track":
        x, rate = read_wav(a.wav)
        x = slice_seconds(x, rate, a.start, a.to)
        tr = f0_track(x, rate, a.fmin, a.fmax)
        summarize(tr, a.wav)
        if a.segments:
            for s, e, m, n in speech_segments(tr):
                print("  segment %.2f..%.2f s: median f0 %.1f Hz (%d voiced hops)" % (s + (a.start or 0), e + (a.start or 0), m, n))
        return 0
    hx, hr = read_wav(a.host); rx, rr = read_wav(a.ref)
    hx = slice_seconds(hx, hr, a.host_from, a.host_to); rx = slice_seconds(rx, rr, a.ref_from, a.ref_to)
    hm = summarize(f0_track(hx, hr, a.fmin, a.fmax), "host"); rm = summarize(f0_track(rx, rr, a.fmin, a.fmax), "ref")
    if hm and rm:
        print("median f0 ratio host/ref = %.4f" % (hm / rm))
    return 0


if __name__ == "__main__":
    sys.exit(main())
