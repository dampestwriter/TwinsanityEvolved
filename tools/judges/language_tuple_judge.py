#!/usr/bin/env python3
"""language_tuple_judge.py -- does ONE language choice drive the text AND the speech AND the FMV sound track?

The retail game has one language index: the text files, the streamed speech bank (Crash6\\<slot>.mb), the FMV sound track (the
movie's channel = the index), a level's voices and the title cards all follow it. The user's PAL image is a text-only patch
(Portuguese text in the `English` slot, the English text in the `Spanish` slot; every .mb/.pss retail), so the first language setting's "pick the
text bank by content and let the index follow it" gave English text with the Spanish speech bank and the Spanish FMV track.
A language is a (text slot, voice slot) tuple derived per disc; this judge checks the three things the user named, per
language, on one deterministic headless run each (fixtures/padlogs/language_tuple.plan: title -> new game -> the intro movie
opens -> skipped -> the hub):

  1 TEXT   the title's text bank is the language's: the runner's exit line `game text 9 = '<line>'` equals the disc's Code
           line 9 of the slot whose text IS that language ("new game", "novo jogo", ...) -- read off the disc by the judge;
  2 FMV    the intro movie's selected sound track (`movie: \\FMV\\H01_A.PSS;1 opened: ... sound track N`, the Player's track id)
           is that language's: the slot NAMED for its speech (English's for a language with no speech bank of its own);
  3 SPEECH the speech bank opened for the in-game lines (`[stream] open #k "Crash6\\<Name>.mb"`, TE_STREAM_TRACE: the stream
           census's bank) is that language's -- the bank every streamed speech line (SetVoiceBank -> StartMusic InVoiceBank)
           comes from;
  + ROWS   the Options rows the runner reports are labelled by what each bank CONTAINS: one per text language ("portuguese
           (english voice)" when its speech is another's), one per speech bank whose language has no text ("spanish (english
           text)" on this disc: the Spanish speech with the English text); no "(... slot)" suffix.

The expected tuple comes from the frozen fixture (fixtures/judges/language_tuple_pal.json, backed by
tools/judges/language_disc_evidence.py: .mb tracks byte-identical to the NTSC-U English bank, title cards identical, the FMV
track's correlation with the NTSC-U track) when the disc's slots match the fixture's, else derived from the disc with the
rule (text slot by content; voice slot = the slot named for the language with a Crash6\\<name>.mb, else English's; a speech bank
whose language has no text adds a row on English's text slot).

Usage: language_tuple_judge.py --bin BIN --iso ISO [--language english|all] [--out DIR] [--fixture F]
Exit 0 = GREEN (every language judged), 1 = RED, 2 = the run failed / the disc has no banks.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "language_tuple_pal.json")

PAL_SLOTS = ["English", "French", "German", "Spanish", "Italian"]
NEW_GAME = {"english": "new game", "portuguese": "novo jogo", "french": "nouvelle partie", "german": "neues spiel",
            "italian": "nuova partita", "spanish": ("nuevo juego", "nueva partida", "juego nuevo"), "dutch": "nieuw spel"}

TEXT9 = re.compile(r"\[twinsanityevolved\] language: .*; game text 9 = '([^']*)'")
MOVIE = re.compile(r"\[twinsanityevolved\] movie: (\S+) opened: .* sound track (\d+) ")
BANK_OPEN = re.compile(r'\[stream\] open #(\d+) "Crash6\\(\w+)\.mb" -> found')
MUSIC_WINDOW = re.compile(r"\[audio\] music window file (\d+) ")
ROW = re.compile(r"row '([^']*)'")
SELECTED_W1 = re.compile(r"requested '([^']*)' -> (.*?) by (.+?): text slot (\d+), voice slot (\d+), Language\(\) (\d+)")
SELECTED_SQ1 = re.compile(r"requested '([^']*)' -> slot (\d+) (\w+) \((\w+)\) by ([^,]+), Language\(\) (\d+)")
GSTATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")


# ---- the disc ------------------------------------------------------------------------------------------------------------
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


def disc_slots(iso):
    """{slot: {name, text, text9, speech, speech_bank}} for every PAL slot the disc carries a Code bank for."""
    with open(iso, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root_lba, root_size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        crash6 = [e for e in iso_dir(f, root_lba, root_size) if e[0].lower() == "crash6" and e[3]]
        if not crash6:
            return {}
        entries = {e[0].upper(): e for e in iso_dir(f, crash6[0][1], crash6[0][2])}
        if "CRASH.BH" not in entries or "CRASH.BD" not in entries:
            return {}
        f.seek(entries["CRASH.BH"][1] * 2048)
        bh = f.read(entries["CRASH.BH"][2])
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
            index[name] = (o, s)
        base = entries["CRASH.BD"][1] * 2048
        slots = {}
        for slot, name in enumerate(PAL_SLOTS):
            key = "Language\\Code\\%s.txt" % name
            if key not in index:
                continue
            o, s = index[key]
            f.seek(base + o)
            lines = [l for l in re.split(r"[\r\n]+", f.read(min(s, 2048)).decode("latin1").lower()) if l]
            line9 = lines[9] if len(lines) > 9 else ""
            content = None
            for lang, words in NEW_GAME.items():
                if line9 in ((words,) if isinstance(words, str) else words):
                    content = lang
            mb = ("%s.MB" % name).upper()
            speech = mb in entries and entries[mb][2] > 0
            slots[slot] = {"name": name, "text": content or name.lower(), "text9": line9, "speech": name.lower() if speech else None,
                           "speech_bank": "Crash6\\%s.mb" % name if speech else None}
        return slots


def derive_languages(slots):
    """The tuple rule over the disc's slots: {language: {text_slot, voice_slot, fmv_track, speech_bank, row}}, english first."""
    out = {}
    for slot in sorted(slots):
        text = slots[slot]["text"]
        if text in out:
            continue
        voice = next((s for s in sorted(slots) if slots[s]["speech"] == text), None)
        if voice is None:
            voice = next((s for s in sorted(slots) if slots[s]["speech"] == "english"), slot)
        vl = slots[voice]["speech"] or slots[voice]["name"].lower()
        out[text] = {"text_slot": slot, "voice_slot": voice, "fmv_track": voice, "speech_bank": slots[voice]["speech_bank"],
                     "row": text + (" (%s voice)" % vl if vl != text else "")}
    order = ["english"] + [k for k in out if k != "english"]
    out = {k: out[k] for k in order}
    # a speech bank whose language has no text: its own voice slot, English's text slot (else its own), "(<text> text)"
    english_text = out["english"]["text_slot"] if "english" in out else None
    for slot in sorted(slots):
        sl = slots[slot]["speech"]
        if sl is None or sl in out:
            continue
        text_slot = english_text if english_text is not None else slot
        out[sl] = {"text_slot": text_slot, "voice_slot": slot, "fmv_track": slot, "speech_bank": slots[slot]["speech_bank"],
                   "row": "%s (%s text)" % (sl, slots[text_slot]["text"])}
    return out


# ---- the run -------------------------------------------------------------------------------------------------------------
def run(binary, iso, out, fx, language):
    d = os.path.join(out, language)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--trace-state", "--language", language,
            "--mc-dir", mc, "--autopilot", os.path.join(ROOT, fx["plan"]), "--stop-at", str(fx.get("stop_at", 12000)),
            "--run-seconds", str(fx.get("run_seconds", 900))]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_STREAM_TRACE"] = "1"
    print("language_tuple_judge: run %s: %s" % (language, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("language_tuple_judge: run %s exit %d -> %s" % (language, rc, log), flush=True)
    return log, rc


def parse_log(path, intro):
    r = {"text9": None, "movies": [], "intro_track": None, "banks": [], "speech_bank": None, "speech_windows": 0, "rows": [],
         "selected": None, "playing": False}
    bank_files = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = TEXT9.search(line)
            if m:
                r["text9"] = m.group(1)
            m = MOVIE.search(line)
            if m:
                r["movies"].append((m.group(1), int(m.group(2))))
                if m.group(1).upper() == intro.upper():
                    r["intro_track"] = int(m.group(2))
            m = BANK_OPEN.search(line)
            if m:
                name = m.group(2)
                if name.lower() != "music":
                    r["banks"].append(name)
                    bank_files[int(m.group(1))] = name
            m = MUSIC_WINDOW.search(line)
            if m and int(m.group(1)) in bank_files:
                r["speech_windows"] += 1
            if "[twinsanityevolved] language:" in line:
                rows = ROW.findall(line)
                if rows:
                    r["rows"] = rows
                m = SELECTED_W1.search(line)
                if m:
                    r["selected"] = {"requested": m.group(1), "language": m.group(2), "how": m.group(3), "text_slot": int(m.group(4)),
                                     "voice_slot": int(m.group(5)), "code": int(m.group(6))}
                m = SELECTED_SQ1.search(line)
                if m:
                    r["selected"] = {"requested": m.group(1), "text_slot": int(m.group(2)), "voice_slot": int(m.group(2)), "how": m.group(5), "code": int(m.group(6))}
            m = GSTATE.search(line)
            if m and m.group(2) == "Playing":
                r["playing"] = True
    # the speech bank the in-game lines stream from: the LAST voice bank opened before the hub (the title's SetVoiceBank; a
    # language change in the Options menu re-opens it)
    r["speech_bank"] = "Crash6\\%s.mb" % r["banks"][-1] if r["banks"] else None
    return r


def judge_one(language, expected, slots, result, fx, rows_expected):
    fails = []
    text_slot = expected["text_slot"]
    want9 = slots[text_slot]["text9"]
    if result["text9"] is None:
        fails.append("the runner printed no `game text 9` line (the run did not end at --stop-at)")
    elif result["text9"] != want9:
        fails.append("TEXT: the title's text 9 is '%s', not the %s bank's '%s' (slot %d)" % (result["text9"], language, want9, text_slot))
    if result["intro_track"] is None:
        fails.append("FMV: the intro movie %s never opened (movies skipped? the plan did not reach NEW GAME?)" % fx["intro_movie"])
    elif result["intro_track"] != expected["fmv_track"]:
        fails.append("FMV: %s opened with sound track %d, not %s's track %d (slot %s %s)" % (
            fx["intro_movie"], result["intro_track"], language, expected["fmv_track"], expected["voice_slot"], slots[expected["voice_slot"]]["name"]))
    if result["speech_bank"] is None:
        fails.append("SPEECH: no Crash6\\<slot>.mb speech bank was opened (TE_STREAM_TRACE missing from the runner?)")
    elif result["speech_bank"].lower() != (expected["speech_bank"] or "").lower():
        fails.append("SPEECH: the speech bank opened is %s, not %s's %s" % (result["speech_bank"], language, expected["speech_bank"]))
    if not result["playing"]:
        fails.append("the run never reached Playing (the hub): the plan did not hold")
    if not result["rows"]:
        fails.append("ROWS: the runner reports no Options rows (`row '...'` on its language line)")
    else:
        if result["rows"] != rows_expected:
            fails.append("ROWS: the Options rows are %s, expected %s" % (result["rows"], rows_expected))
        bad = [r for r in result["rows"] if "slot)" in r or (r == "spanish" and not any(s["text"] == "spanish" for s in slots.values()))]
        if bad:
            fails.append("ROWS: rows labelled by slot, not content: %s" % bad)
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--language", default="english", help="one language, or `all` = every language the disc carries (default english)")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/language_tuple_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or os.path.join(ROOT, "build", "language_tuple_judge")
    os.makedirs(out, exist_ok=True)

    slots = disc_slots(a.iso)
    if not slots:
        print("language_tuple_judge: the disc carries no Language\\Code banks the judge can read", file=sys.stderr)
        return 2
    fx_slots = {int(k): v for k, v in fx["slots"].items()}
    same_disc = all(k in slots and slots[k]["text"] == v["text"] and slots[k]["speech"] == v["speech"] for k, v in fx_slots.items()) and len(slots) == len(fx_slots)
    if same_disc:
        languages = fx["languages"]
        rows_expected = fx["rows"]
        print("language_tuple_judge: the disc's slots match the frozen fixture (%s): its evidence-backed tuples stand" % os.path.relpath(a.fixture, ROOT))
    else:
        languages = derive_languages(slots)
        rows_expected = [languages[k]["row"] for k in languages]
        print("language_tuple_judge: another disc than the fixture's: the tuples are derived from its slots (the tuple rule)")
    for slot in sorted(slots):
        print(json.dumps({"disc_slot": slot, **slots[slot]}))
    for lang, t in languages.items():
        print(json.dumps({"language": lang, **t, "text9": slots[t["text_slot"]]["text9"]}))

    wanted = list(languages) if a.language.lower() == "all" else [a.language.lower()]
    for lang in wanted:
        if lang not in languages:
            print("language_tuple_judge: the disc has no language '%s' (it has %s)" % (lang, ", ".join(languages)), file=sys.stderr)
            return 2

    verdicts = {}
    all_fails = []
    for lang in wanted:
        log, rc = run(a.bin, a.iso, out, fx, lang)
        if rc != 0:
            print("language_tuple_judge: run %s exited %d" % (lang, rc), file=sys.stderr)
            return 2
        result = parse_log(log, fx["intro_movie"])
        fails = judge_one(lang, languages[lang], slots, result, fx, rows_expected)
        verdicts[lang] = {"expected": languages[lang], "text9": result["text9"], "intro_track": result["intro_track"], "speech_bank": result["speech_bank"],
                          "speech_windows": result["speech_windows"], "banks_opened": result["banks"], "rows": result["rows"], "selected": result["selected"],
                          "verdict": "green" if not fails else "red", "reasons": fails}
        print(json.dumps({"run": lang, **verdicts[lang]}))
        all_fails += ["%s: %s" % (lang, f) for f in fails]

    verdict = "GREEN" if not all_fails else "RED"
    print(json.dumps({"verdict": verdict.lower(), "languages": wanted, "bin": a.bin, "reasons": all_fails}))
    print("language_tuple_judge: %s (%s)" % (verdict, ", ".join("%s %s" % (l, verdicts[l]["verdict"]) for l in wanted)))
    return 0 if not all_fails else 1


if __name__ == "__main__":
    sys.exit(main())
