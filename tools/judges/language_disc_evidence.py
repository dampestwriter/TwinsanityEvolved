#!/usr/bin/env python3
"""language_disc_evidence.py -- what each language SLOT of a Crash Twinsanity disc actually holds, component by
component, with the NTSC-U disc (English only) as the listening-equivalent reference. Nothing is extracted to disk.

Per slot (the PAL executable's order English, French, German, Spanish, Italian):
  text       Language\\Code\\<slot>.txt line 9 (the main menu's "new game") and the language it is in; the AgentLab (the in-game
             dialogue/subtitle text) first distinctive line
  titles     Language\\Titles\\<slot>\\*.psm (the hub/level title cards): identical to the NTSC American ones? (bytes)
  credits    Language\\Credits\\<slot>.txt: a tell-tale line
  speech     Crash6\\<slot>.mb (the streamed speech bank SetVoiceBank opens): how many of its tracks are byte-identical to the NTSC
             CRASH6\\AMERICAN.MB's (the English speech) -- the English bank matches, the others differ on the real lines
  fmv        FMV/H01_A.PSS (the intro movie) sound track <slot>: loudness-envelope correlation with the NTSC movie's one track
             after the 1.2x PAL stretch (PAL plays the 29.97 fps master at 25 fps), on the speech residual (track minus the
             tracks' median: the music/effects bed cancels) -- the English track correlates ~0.7-0.8, the others ~0.1

Usage: language_disc_evidence.py --pal ISO --ntsc ISO [--movie FMV/H01_A.PSS]
"""
import argparse
import hashlib
import re
import struct
import sys

try:
    import numpy as np
except ImportError:  # the FMV correlation needs numpy; the rest does not
    np = None

SLOTS = ["English", "French", "German", "Spanish", "Italian"]
NEW_GAME = {"english": "new game", "portuguese": "novo jogo", "french": "nouvelle partie", "german": "neues spiel",
            "italian": "nuova partita", "spanish": ("nuevo juego", "nueva partida", "juego nuevo"), "dutch": "nieuw spel"}


# ---- ISO9660 ----------------------------------------------------------------------------------------------------------------
def iso_dir(f, lba, size):
    f.seek(lba * 2048)
    d = f.read(size)
    out, off = [], 0
    while off < len(d):
        ln = d[off]
        if ln == 0:
            off = (off // 2048 + 1) * 2048
            continue
        e_lba, e_size = struct.unpack_from("<I", d, off + 2)[0], struct.unpack_from("<I", d, off + 10)[0]
        nl = d[off + 32]
        name = d[off + 33:off + 33 + nl].decode("latin1").split(";")[0]
        out.append((name, e_lba, e_size, d[off + 25] & 2))
        off += ln
    return out


def iso_find(f, path):
    f.seek(16 * 2048)
    pvd = f.read(2048)
    lba, size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
    for part in path.replace("\\", "/").split("/"):
        hit = [e for e in iso_dir(f, lba, size) if e[0].lower() == part.lower()]
        if not hit:
            return None
        _, lba, size, _ = hit[0]
    return lba, size


def iso_read(f, path, limit=None):
    hit = iso_find(f, path)
    if not hit:
        return None
    lba, size = hit
    f.seek(lba * 2048)
    return f.read(size if limit is None else min(size, limit))


# ---- the archive index CRASH6/CRASH.BH -> CRASH.BD ---------------------------------------------------------------------------
def archive(f):
    bh = iso_read(f, "CRASH6/CRASH.BH")
    bd = iso_find(f, "CRASH6/CRASH.BD")
    if not bh or not bd:
        return {}
    base = bd[0] * 2048
    index, off = {}, 4
    while off + 4 <= len(bh):
        nl = struct.unpack_from("<I", bh, off)[0]
        off += 4
        if nl > 512 or off + nl + 8 > len(bh):
            break
        name = bh[off:off + nl].decode("latin1")
        off += nl
        o, s = struct.unpack_from("<II", bh, off)
        off += 8
        index[name] = (base + o, s)
    return index


def archive_read(f, index, name, limit=None):
    if name not in index:
        return None
    o, s = index[name]
    f.seek(o)
    return f.read(s if limit is None else min(s, limit))


def text_lines(b):
    return [l for l in re.split(r"[\r\n]+", b.decode("latin1")) if l.strip()]


def content_of(line9):
    for lang, words in NEW_GAME.items():
        words = (words,) if isinstance(words, str) else words
        if line9.lower() in words:
            return lang
    return None


# ---- the speech banks: <slot>.mh track table + <slot>.mb bytes ----------------------------------------------------------------
def mh_tracks(f, path):
    d = iso_read(f, path)
    if not d:
        return None
    n = struct.unpack_from("<I", d, 0)[0]
    return [struct.unpack_from("<IIIII", d, 8 + 20 * i) for i in range(n)]   # kind, size, offset, rate, value


def mb_track_hashes(f, mh, mb, limit=1 << 16):
    tracks = mh_tracks(f, mh)
    hit = iso_find(f, mb)
    if not tracks or not hit:
        return None
    base = hit[0] * 2048
    out = []
    for kind, size, offset, rate, value in tracks:
        f.seek(base + offset)
        out.append((size, hashlib.sha256(f.read(min(size, limit))).hexdigest()))
    return out


# ---- the movie's sound tracks (private_stream_1, FF A0 cc cc, SShd/SSbd raw PCM16LE 48 kHz stereo) --------------------------
def pss_tracks(f, path, limit=1 << 30):
    hit = iso_find(f, path)
    if not hit:
        return {}
    lba, size = hit
    f.seek(lba * 2048)
    d = f.read(min(size, limit))
    tracks, i, n = {}, 0, len(d)
    while i + 4 <= n:
        if d[i] != 0 or d[i + 1] != 0 or d[i + 2] != 1:
            i += 1
            continue
        sid = d[i + 3]
        if sid == 0xBA:
            if i + 14 > n:
                break
            i += 14 + (d[i + 13] & 7)
            continue
        if sid == 0xB9:
            break
        if i + 6 > n:
            break
        length = struct.unpack_from(">H", d, i + 4)[0]
        start, end = i + 6, i + 6 + length
        if sid == 0xBD and end <= n:
            p = start + 3 + d[start + 2]
            if d[p] == 0xFF and d[p + 1] == 0xA0:
                tracks.setdefault((d[p + 2] << 8) | d[p + 3], bytearray()).extend(d[p + 4:end])
        i = end
    bodies = {}
    for ch, t in tracks.items():
        if t[:4] != b"SShd":
            continue
        hl = struct.unpack_from("<I", t, 4)[0]
        bodies[ch] = bytes(t[8 + hl + 8:])
    return bodies


def envelope(body, rate=48000, hz=100):
    n = len(body) // 4
    s = np.frombuffer(body[:n * 4], dtype="<i2").reshape(-1, 2).astype(np.float32).mean(axis=1)
    win = rate // hz
    m = len(s) // win
    return np.sqrt((s[:m * win].reshape(m, win) ** 2).mean(axis=1))


def best_lag(a, b, max_lag):
    a = (a - a.mean()) / (a.std() + 1e-9)
    b = (b - b.mean()) / (b.std() + 1e-9)
    best = (-2.0, 0)
    for lag in range(-max_lag, max_lag + 1):
        x, y = (a[lag:], b[:len(b) - lag]) if lag >= 0 else (a[:len(a) + lag], b[-lag:])
        m = min(len(x), len(y))
        if m < 100:
            continue
        c = float((x[:m] * y[:m]).mean())
        if c > best[0]:
            best = (c, lag)
    return best


def fmv_correlation(pal, ntsc, movie):
    """{pal track: (envelope corr vs the stretched NTSC track, speech-residual corr)}; None without numpy / the movie."""
    if np is None:
        return None
    with open(pal, "rb") as f:
        pal_tracks = pss_tracks(f, movie)
    with open(ntsc, "rb") as f:
        ntsc_tracks = pss_tracks(f, movie)
    if not pal_tracks or 0 not in ntsc_tracks:
        return None
    ref = ntsc_tracks[0]
    ratio = len(pal_tracks[min(pal_tracks)]) / float(len(ref))
    en = envelope(ref)
    xs = np.arange(int(len(en) * ratio)) / ratio
    en_s = np.interp(xs, np.arange(len(en)), en)
    ep = {ch: envelope(b) for ch, b in pal_tracks.items()}
    m = min(len(en_s), *[len(e) for e in ep.values()])
    en_s = en_s[:m]
    ep = {ch: e[:m] for ch, e in ep.items()}
    med = np.median(np.stack([ep[ch] for ch in sorted(ep)]), axis=0)
    out = {"ratio": ratio, "seconds": m / 100.0, "tracks": {}}
    for ch in sorted(ep):
        c_env, _ = best_lag(ep[ch], en_s, 200)
        c_res, _ = best_lag(ep[ch] - med, en_s - med, 200)
        out["tracks"][ch] = (c_env, c_res)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pal", required=True)
    ap.add_argument("--ntsc", required=True)
    ap.add_argument("--movie", default="FMV/H01_A.PSS")
    a = ap.parse_args()
    with open(a.pal, "rb") as fp, open(a.ntsc, "rb") as fn:
        ip, inn = archive(fp), archive(fn)
        print("PAL %s: %d archive entries; NTSC %s: %d" % (a.pal.split("/")[-1], len(ip), a.ntsc.split("/")[-1], len(inn)))
        am_titles = {k.split("\\")[-1]: archive_read(fn, inn, k) for k in inn if k.startswith("Language\\Titles\\American\\")}
        am_speech = mb_track_hashes(fn, "CRASH6/AMERICAN.MH", "CRASH6/AMERICAN.MB")
        am_agent = text_lines(archive_read(fn, inn, "Language\\AgentLab\\American.txt") or b"")
        am_agent = [l for l in am_agent if l.strip() != "zzz"][:1]
        print("NTSC American: Code text 9 '%s'; AgentLab '%s'; %d title cards; speech bank %s tracks" % (
            text_lines(archive_read(fn, inn, "Language\\Code\\American.txt") or b"")[9], am_agent[0] if am_agent else "", len(am_titles),
            len(am_speech) if am_speech else "no"))
        fmv = fmv_correlation(a.pal, a.ntsc, a.movie)
        if fmv:
            print("%s: %d PAL sound tracks, %.1f s, PAL/NTSC length ratio %.4f (the PAL stretch)" % (a.movie, len(fmv["tracks"]), fmv["seconds"], fmv["ratio"]))
        print()
        print("%-4s %-8s %-18s %-14s %-24s %-22s %-26s %s" % ("slot", "name", "Code text (line 9)", "AgentLab", "Titles vs NTSC American", "Credits", "speech .mb vs NTSC American", "FMV track vs NTSC (env / speech)"))
        for slot, name in enumerate(SLOTS):
            code = archive_read(fp, ip, "Language\\Code\\%s.txt" % name)
            if code is None:
                print("%-4d %-8s (no Code bank)" % (slot, name))
                continue
            l9 = text_lines(code)[9]
            agent = [l for l in text_lines(archive_read(fp, ip, "Language\\AgentLab\\%s.txt" % name) or b"") if l.strip() != "zzz"]
            credits = text_lines(archive_read(fp, ip, "Language\\Credits\\%s.txt" % name) or b"")
            tell = next((l for l in credits if "thanks" in l.lower() or "remerciements" in l.lower() or "dank" in l.lower()
                         or "agradecimientos" in l.lower() or "ringraziamenti" in l.lower()), credits[1] if len(credits) > 1 else "")
            same = sum(1 for k, v in am_titles.items() if archive_read(fp, ip, "Language\\Titles\\%s\\%s" % (name, k)) == v)
            speech = mb_track_hashes(fp, "CRASH6/%s.MH" % name, "CRASH6/%s.MB" % name)
            if speech and am_speech:
                ident = sum(1 for x, y in zip(speech, am_speech) if x == y)
                sp = "%d/%d tracks identical" % (ident, len(speech))
            else:
                sp = "no .mb" if not speech else "no NTSC reference"
            fm = "%.2f / %.2f" % fmv["tracks"][slot] if fmv and slot in fmv["tracks"] else "-"
            print("%-4d %-8s %-18s %-14s %-24s %-22s %-26s %s" % (slot, name, "%s (%s)" % (l9, content_of(l9) or "?"), (agent[0] if agent else "")[:14],
                                                                   "%d/%d identical" % (same, len(am_titles)), tell[:22], sp, fm))
    return 0


if __name__ == "__main__":
    sys.exit(main())
