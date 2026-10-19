#!/usr/bin/env python3
"""language_table_judge.py -- is EVERY language the disc carries (text or speech) in the language table, Spanish included?

The user's PAL image is a text-only patch: the `English` slot's text is Portuguese and the `Spanish` slot's text is English, every
speech bank (Crash6\\<Slot>.mb), FMV track and title card is the 2004 retail file. The first tuple table offered one row per TEXT language
and so had no Spanish: the Spanish speech, title cards and credits were unreachable. This judge checks, against the disc itself:

  A  EVIDENCE   ten strings per slot off the disc (Language\\Code\\<slot>.txt lines 9-15: the title menu and the Options rows;
                Language\\AgentLab\\<slot>.txt lines 0, 4, 7: the credits' thank-you and the first level's dialogue) and the
                language they are in, decided by a per-language word table over all ten (not the one "new game" string). The
                table is printed; the fixture's per-slot verdict must agree with it (the fixture is frozen FROM this evidence).
  B  TABLE      a run's language line lists one row per text language the disc carries PLUS one per speech bank whose language
                has no text, labelled by content ("spanish (english text)", "portuguese (english voice)"); a spanish row exists.
  C  SELECTION  one deterministic headless run per disc language (the five slot languages: english, french, german, spanish,
                italian) with `--language <name>` to the Title: the runner selects it BY NAME (not "no language ... : english"),
                the speech bank opened is that language's Crash6\\<Slot>.mb, the Code text the game ended in (`game text 9`) is
                the language's text slot's line 9, GameController reached Title (7).
  D  MENU       one run playing fixtures/padlogs/language_table_rows.padlog (title -> OPTIONS -> GAME OPTIONS -> the language row
                stepped RIGHT seven times): the Options row cycles through EVERY row of the table, spanish included, and the
                spanish step opens Crash6\\Spanish.mb.

RED on bin/twinsanityevolved.310b6f0 (no spanish row: C's spanish run falls back to english, D cycles five rows), GREEN after.

Usage: language_table_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--evidence-only]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / the disc has no banks.
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "language_table_pal.json")

PAL_SLOTS = ["English", "French", "German", "Spanish", "Italian"]
CODE_LINES = [9, 10, 11, 12, 13, 14, 15]          # new game, load game, options, language, graphic / sound / game options
AGENT_LINES = [0, 4, 7]                           # thanks for playing; "you gotta help me crash"; "what did i do to deserve this"

# The word table: a string is in a language when it starts with one of that language's words (lower case; accents as the disc
# writes them in Latin-1, matched on their ASCII prefix where they differ)
WORDS = {
    "english": ["new game", "load game", "options", "language", "graphic options", "sound options", "game options",
                "thanks for playing", "you gotta help me", "what did i do"],
    "portuguese": ["novo jogo", "carregar jogo", "op\xe7\xf5es", "idioma", "op\xe7\xf5es gr", "op\xe7\xf5es de som", "op\xe7\xf5es do jogo",
                   "obrigado por jogar", "voc\xea tem que me ajudar", "o que eu fiz"],
    "french": ["nouvelle partie", "charger", "options", "langue", "options graphiques", "options sonores", "options de jeu",
               "merci d'avoir jou", "tu dois m'aider", "qu'est-ce que j'ai fait"],
    "german": ["neues spiel", "spiel laden", "optionen", "sprache", "grafik-optionen", "sound-optionen", "spiel-optionen",
               "danke f\xfcrs spielen", "du musst mir helfen", "womit hab ich das verdient"],
    "italian": ["nuova partita", "carica partita", "opzioni", "lingua", "opzioni grafiche", "opzioni audio", "opzioni di gioco",
                "grazie per aver giocato", "devi aiutarmi", "cosa ho fatto"],
    "spanish": ["nuevo juego", "nueva partida", "juego nuevo", "cargar partida", "cargar juego", "opciones", "idioma", "opciones gr",
                "opciones de sonido", "opciones de juego", "gracias por jugar", "tienes que ayudarme", "qu\xe9 he hecho", "qu\xe9 hice"],
}

LANGLINE = re.compile(r"\[twinsanityevolved\] language: (.*)")
ROW = re.compile(r"\[(\w+): text slot (\d+)(?: \((\w+)\))?, voice slot (\d+) \((\w+)\), row '([^']*)'\]")
SELECTED_BY = re.compile(r"requested '([^']*)' -> (.*?) by (.+?): text slot (\d+), voice slot (\d+), Language\(\) (\d+)")
TEXT9 = re.compile(r"game text 9 = '([^']*)'")
BANK_OPEN = re.compile(r'\[stream\] open #(\d+) "Crash6\\(\w+)\.mb" -> found')
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


def text_lines(b):
    return [l for l in re.split(r"[\r\n]+", b.decode("latin1").lower()) if l.strip()]


def language_of(strings):
    """(language, hits) by the word table over the strings: the most hits wins, at least half of them."""
    best, best_hits = None, 0
    for lang, words in WORDS.items():
        hits = sum(1 for s in strings if any(s.startswith(w) for w in words))
        if hits > best_hits:
            best, best_hits = lang, hits
    return (best if best_hits * 2 >= len(strings) else None), best_hits


def disc_slots(iso):
    """{slot: {name, strings (10), text (the language of the ten), hits, text9, speech, speech_bank}} per PAL slot with a Code bank."""
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

        def read(name):
            if name not in index:
                return b""
            o, s = index[name]
            f.seek(base + o)
            return f.read(s)

        slots = {}
        for slot, name in enumerate(PAL_SLOTS):
            code = text_lines(read("Language\\Code\\%s.txt" % name))
            if not code:
                continue
            agent = [l for l in text_lines(read("Language\\AgentLab\\%s.txt" % name)) if l.strip() != "zzz"]
            strings = [code[i] if i < len(code) else "" for i in CODE_LINES] + [agent[i] if i < len(agent) else "" for i in AGENT_LINES]
            text, hits = language_of(strings)
            mb = ("%s.MB" % name).upper()
            speech = mb in entries and entries[mb][2] > 0
            slots[slot] = {"name": name, "strings": strings, "text": text or name.lower(), "recognised": text is not None, "hits": hits,
                           "text9": code[9] if len(code) > 9 else "", "speech": name.lower() if speech else None,
                           "speech_bank": "Crash6\\%s.mb" % name if speech else None}
        return slots


def derive_table(slots):
    """The expected table: {language: {text_slot, voice_slot, speech_bank, row}} -- one per text language (english first), the
    voice slot the slot NAMED for it with a speech bank (else English's); then one per speech bank whose language has no text,
    on English's text slot (else its own), labelled '(<text> text)'."""
    out = {}
    for slot in sorted(slots):
        text = slots[slot]["text"]
        if text in out:
            continue
        voice = next((s for s in sorted(slots) if slots[s]["speech"] == text), None)
        if voice is None:
            voice = next((s for s in sorted(slots) if slots[s]["speech"] == "english"), slot)
        vl = slots[voice]["speech"] or slots[voice]["name"].lower()
        out[text] = {"text_slot": slot, "voice_slot": voice, "speech_bank": slots[voice]["speech_bank"],
                     "row": text + (" (%s voice)" % vl if vl != text else "")}
    order = ["english"] + [k for k in out if k != "english"]
    out = {k: out[k] for k in order}
    english_text = out["english"]["text_slot"] if "english" in out else None
    for slot in sorted(slots):
        sl = slots[slot]["speech"]
        if sl is None or sl in out:
            continue
        text_slot = english_text if english_text is not None else slot
        out[sl] = {"text_slot": text_slot, "voice_slot": slot, "speech_bank": slots[slot]["speech_bank"],
                   "row": "%s (%s text)" % (sl, slots[text_slot]["text"])}
    return out


# ---- the runs ------------------------------------------------------------------------------------------------------------
def run(binary, iso, out, tag, extra, stop_at, run_seconds):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--trace-state", "--mc-dir", mc,
            "--stop-at", str(stop_at), "--run-seconds", str(run_seconds)] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_STREAM_TRACE"] = "1"
    print("language_table_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("language_table_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return log, rc


def parse_log(path):
    """The language lines in order (rows, the selection each announces), the speech banks opened in order (with the line
    number), the game's text 9 at exit, whether Title (7) was reached."""
    r = {"lines": [], "banks": [], "text9": None, "title": False, "rows": None}
    with open(path, errors="replace") as f:
        for n, line in enumerate(f):
            m = LANGLINE.search(line)
            if m:
                body = m.group(1)
                rows = [(x[0], int(x[1]), x[2] or x[0], int(x[3]), x[4], x[5]) for x in ROW.findall(body)]
                sel = SELECTED_BY.search(body)
                entry = {"n": n, "rows": [x[5] for x in rows],
                         "table": {x[0]: {"text_slot": x[1], "text_language": x[2], "voice_slot": x[3], "voice_language": x[4], "row": x[5]} for x in rows},
                         "selected": None}
                if sel:
                    entry["selected"] = {"requested": sel.group(1), "row": sel.group(2), "how": sel.group(3), "text_slot": int(sel.group(4)),
                                         "voice_slot": int(sel.group(5)), "code": int(sel.group(6))}
                r["lines"].append(entry)
                if r["rows"] is None and rows:
                    r["rows"] = entry["rows"]
                m9 = TEXT9.search(body)
                if m9:
                    r["text9"] = m9.group(1)
            m = BANK_OPEN.search(line)
            if m and m.group(2).lower() != "music":
                r["banks"].append((n, m.group(2)))
            m = GSTATE.search(line)
            if m and int(m.group(3)) == 7:
                r["title"] = True
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/language_table_judge)")
    ap.add_argument("--evidence-only", action="store_true", help="print the ten-strings-per-slot table and the derived table; no runs")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or os.path.join(ROOT, "build", "language_table_judge")
    os.makedirs(out, exist_ok=True)

    # A -- the evidence
    slots = disc_slots(a.iso)
    if not slots:
        print("language_table_judge: the disc carries no Language\\Code banks the judge can read", file=sys.stderr)
        return 2
    fails = []
    print("language_table_judge: ten strings per slot (Code lines %s; AgentLab lines %s)" % (CODE_LINES, AGENT_LINES))
    for slot in sorted(slots):
        s = slots[slot]
        print("  slot %d %-8s text=%s (%d/10 words) speech=%s" % (slot, s["name"], s["text"], s["hits"], s["speech"]))
        for i, st in enumerate(s["strings"]):
            print("      %s %s" % ("C%d" % CODE_LINES[i] if i < len(CODE_LINES) else "A%d" % AGENT_LINES[i - len(CODE_LINES)], st[:70]))
    fx_slots = {int(k): v for k, v in fx["slots"].items()}
    same_disc = len(slots) == len(fx_slots) and all(k in slots and slots[k]["text"] == v["text"] and slots[k]["speech"] == v["speech"] for k, v in fx_slots.items())
    if same_disc:
        table = fx["languages"]
        print("language_table_judge: the disc's slots match the frozen fixture (%s): its table stands" % os.path.relpath(a.fixture, ROOT))
    else:
        table = derive_table(slots)
        print("language_table_judge: another disc than the fixture's: the table is derived from its slots")
    rows_expected = [table[k]["row"] for k in table]
    for lang, t in table.items():
        print(json.dumps({"language": lang, **t, "text9": slots[t["text_slot"]]["text9"]}))
    if not any(l == "spanish" for l in table):
        print("language_table_judge: NOTE the disc has neither Spanish text nor a Spanish speech bank: no spanish row is expected")
    if a.evidence_only:
        return 0

    # B + C -- one run per disc language
    languages = fx.get("select", [l for l in table])
    verdicts = {}
    for lang in languages:
        if lang not in table:
            fails.append("%s: the disc has no such language (table %s)" % (lang, list(table)))
            continue
        exp = table[lang]
        log, rc = run(a.bin, a.iso, out, lang, ["--language", lang], fx.get("stop_at", 2500), fx.get("run_seconds", 300))
        if rc != 0:
            print("language_table_judge: run %s exited %d" % (lang, rc), file=sys.stderr)
            return 2
        r = parse_log(log)
        f = []
        if r["rows"] is None:
            f.append("TABLE: the runner printed no language table")
        elif r["rows"] != rows_expected:
            f.append("TABLE: rows %s, expected %s" % (r["rows"], rows_expected))
        if r["rows"] is not None and not any(x.startswith("spanish") for x in r["rows"]) and "spanish" in table:
            f.append("TABLE: no spanish row")
        sel = r["lines"][0]["selected"] if r["lines"] else None
        if sel is None:
            f.append("SELECTION: the runner reported no selection")
        else:
            if sel["how"] != "name":
                f.append("SELECTION: '%s' selected by %s, not by name (-> %s)" % (lang, sel["how"], sel["row"]))
            if sel["text_slot"] != exp["text_slot"] or sel["voice_slot"] != exp["voice_slot"]:
                f.append("SELECTION: text slot %d / voice slot %d, expected %d / %d" % (sel["text_slot"], sel["voice_slot"], exp["text_slot"], exp["voice_slot"]))
        bank = "Crash6\\%s.mb" % r["banks"][-1][1] if r["banks"] else None
        if (bank or "").lower() != (exp["speech_bank"] or "").lower():
            f.append("SPEECH: the speech bank opened is %s, expected %s" % (bank, exp["speech_bank"]))
        want9 = slots[exp["text_slot"]]["text9"]
        if r["text9"] != want9:
            f.append("TEXT: game text 9 is %r, expected the slot %d text's %r" % (r["text9"], exp["text_slot"], want9))
        if not r["title"]:
            f.append("the run did not reach Title (7)")
        verdicts[lang] = {"rows": r["rows"], "selected": sel, "speech_bank": bank, "text9": r["text9"], "title": r["title"],
                          "verdict": "green" if not f else "red", "reasons": f}
        print(json.dumps({"run": lang, **verdicts[lang]}))
        fails += ["%s: %s" % (lang, x) for x in f]

    # D -- the Options menu steps through every row
    log, rc = run(a.bin, a.iso, out, "menu", ["--skip-movies", "--language", fx.get("menu_start", "english"), "--play-pad",
                                               os.path.join(ROOT, fx["menu_padlog"])], fx.get("menu_stop_at", 2000), fx.get("run_seconds", 300))
    if rc != 0:
        print("language_table_judge: the menu run exited %d" % rc, file=sys.stderr)
        return 2
    r = parse_log(log)
    steps = [(e["n"], e["selected"]["requested"], e["selected"]["voice_slot"]) for e in r["lines"] if e["selected"] and e["selected"]["how"] == "options menu"]
    stepped = []
    for n, name, voice in steps:
        if name not in stepped:
            stepped.append(name)
    f = []
    if not steps:
        f.append("MENU: no Options-menu language step was logged (the pad log did not reach the row?)")
    missing = [l for l in table if l not in stepped]
    if missing:
        f.append("MENU: the Options row never showed %s (stepped: %s)" % (missing, stepped))
    if "spanish" in table:
        es = [n for n, name, voice in steps if name == "spanish"]
        if es:
            after = [b for (n, b) in r["banks"] if n > es[0]]
            if not after or after[0].lower() != "spanish":
                f.append("MENU: the spanish step did not open Crash6\\Spanish.mb (banks after it: %s)" % after[:2])
    verdicts["menu"] = {"steps": [(name, voice) for n, name, voice in steps], "banks": [b for n, b in r["banks"]], "verdict": "green" if not f else "red", "reasons": f}
    print(json.dumps({"run": "menu", **verdicts["menu"]}))
    fails += ["menu: %s" % x for x in f]

    verdict = "GREEN" if not fails else "RED"
    print(json.dumps({"verdict": verdict.lower(), "bin": a.bin, "languages": languages, "rows_expected": rows_expected, "reasons": fails}))
    print("language_table_judge: %s (%s)" % (verdict, ", ".join("%s %s" % (k, v["verdict"]) for k, v in verdicts.items())))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
