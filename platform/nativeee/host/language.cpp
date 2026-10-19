// platform/nativeee (host side) -- te_language.h: the game's language as a setting (one choice drives text
// AND speech; a speech-only language is offered with English's text). See the header for the model.
//
// Enumeration: the slot names come from the retail image in guest memory (GlobalLanguagesArray, a NUL-terminated list of
// pointers to the names, PAL 0x002E6F00 -- the game's g_GameLanguageNames the executable carries), the banks from the disc's
// archive index CRASH6/CRASH.BH (u32 count; then per entry u32 name length, the name, u32 offset, u32 size -- the game's own
// FileStreamOpenArchive reads the same), the first bytes of each Language\Code\<name>.txt out of CRASH6/CRASH.BD, and the
// speech banks Crash6\<name>.mb from the disc's directory. Nothing is extracted to disk (charter: game data is read at run time
// only).
#include "te_language.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "engine/cdvd/iso_fs.h"
#include "engine/core/config/settings_file.h"
#include "te_host.h"
#include "te_services.h"

namespace te::language {

namespace {

constexpr uint32_t kGlobalLanguagesArray = 0x002E6F00;   // game/symbol_addrs.txt GlobalLanguagesArray (PAL SLES-52568)
constexpr uint32_t kMaxSlots = 16;
constexpr size_t kProbeBytes = 2048;                     // the Code bank's first menu lines lie well inside

// The PAL executable's slot order (the fallback when the retail image is not readable: tests, tools)
const char* const kPalSlots[] = {"English", "French", "German", "Spanish", "Italian"};

// The console language the game's GameLanguageOf (game/src/game/gamecontext.cpp) maps to a slot of that NAME: French 2,
// Spanish 3, German 4, Italian 5, everything else English's slot (1). A slot the game has no case for (none on either disc)
// cannot be selected by a code and reads 1.
int32_t CodeOfSlotName(const std::string& name) {
    if (name == "French") return 2;
    if (name == "Spanish") return 3;
    if (name == "German") return 4;
    if (name == "Italian") return 5;
    return 1;
}

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// The language a slot NAME means ("American" is English)
std::string LanguageOfSlotName(const std::string& name) {
    const std::string l = Lower(name);
    return l == "american" ? "english" : l;
}

// The recognition table: a language's tell-tale menu words (the Code bank's first lines: off/on/no/yes/back/continue, then
// new game / load game / options / language / graphic options ...). Lower case, ASCII only where the words differ by
// accents (UTF-8 and the title's Latin-1 both pass the ASCII parts).
struct Tell { const char* language; const char* const* words; size_t count; };
const char* const kEnglish[] = {"new game", "load game", "graphic options", "sound options", "game options", "widescreen"};
const char* const kPortuguese[] = {"novo jogo", "carregar jogo", "op", "idioma", "desativar", "ativar", "voltar", "continuar"};
const char* const kFrench[] = {"nouvelle partie", "charger", "langue", "options graphiques", "options sonores", "retour", "continuer"};
const char* const kGerman[] = {"neues spiel", "spiel laden", "optionen", "sprache", "grafik-optionen", "sound-optionen", "aus", "ein", "nein", "ja"};
const char* const kItalian[] = {"nuova partita", "carica partita", "opzioni", "lingua", "opzioni grafiche", "opzioni audio", "indietro", "continua"};
const char* const kSpanish[] = {"nuevo juego", "nueva partida", "cargar", "opciones", "idioma", "opciones gr", "atr", "volver", "continuar"};
const char* const kDutch[] = {"nieuw spel", "spel laden", "opties", "taal", "terug", "verder", "uit", "aan", "nee"};
const Tell kTells[] = {
    {"english", kEnglish, sizeof kEnglish / sizeof *kEnglish},
    {"portuguese", kPortuguese, sizeof kPortuguese / sizeof *kPortuguese},
    {"french", kFrench, sizeof kFrench / sizeof *kFrench},
    {"german", kGerman, sizeof kGerman / sizeof *kGerman},
    {"italian", kItalian, sizeof kItalian / sizeof *kItalian},
    {"spanish", kSpanish, sizeof kSpanish / sizeof *kSpanish},
    {"dutch", kDutch, sizeof kDutch / sizeof *kDutch},
};

std::vector<std::string> Lines(const std::string& text, size_t at_most) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) { out.push_back(Lower(cur)); cur.clear(); if (out.size() >= at_most) break; }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && out.size() < at_most) out.push_back(Lower(cur));
    return out;
}

Table g_table;
bool g_resolved = false;

std::vector<std::string> SlotNamesFromImage() {
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        const uint8_t* p = GuestPointer(kGlobalLanguagesArray + i * 4, 4);
        if (!p) break;
        uint32_t ptr = 0;
        std::memcpy(&ptr, p, 4);
        if (ptr == 0) break;
        const char* s = GuestString(ptr, 32);
        if (!s || !*s) break;
        // A name is letters only (the retail array's 5 pointers are followed by other data on another executable)
        bool letters = true;
        for (const char* c = s; *c; ++c) letters = letters && ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'));
        if (!letters) break;
        names.emplace_back(s);
    }
    return names;
}

const Bank* BankOfSlot(const std::vector<Bank>& banks, int32_t slot) {
    for (const Bank& b : banks) if (int32_t(b.slot) == slot) return &b;
    return nullptr;
}

// A raw language entry: slot N for everything, as the retail game ran a console set to that slot
Language RawLanguage(const std::vector<Bank>& banks, int32_t slot, const std::string& why) {
    Language l;
    l.raw = true;
    l.text_slot = l.voice_slot = slot;
    const Bank* b = BankOfSlot(banks, slot);
    l.name = b ? b->content : "slot " + std::to_string(slot);
    l.text_language = l.name;
    l.voice_language = b ? b->speech_language : "";
    l.label = "slot " + std::to_string(slot) + (b ? " " + Lower(b->name) : "") + " (" + why + ")";
    return l;
}

void Select(Table& t) {
    const Settings& s = GetSettings();
    t.requested = Lower(s.language_name.empty() ? "english" : s.language_name);
    t.selected = -1;
    t.how.clear();
    if (t.banks.empty()) return;
    // 1 a raw slot (--language-bank N): text = voice = the slot, exactly what retail did for a console set to it (the judges'
    //   "as retail" arm; the pad logs' `# language-bank=N` pin)
    if (s.language_bank >= 0) {
        if (BankOfSlot(t.banks, s.language_bank)) {
            t.languages.push_back(RawLanguage(t.banks, s.language_bank, "raw"));
            t.selected = int32_t(t.languages.size()) - 1;
            t.how = "raw slot " + std::to_string(s.language_bank);
            return;
        }
    }
    // 2 a raw console code (--language 0..7, the raw-code form): the slot the game's GameLanguageOf maps it to, raw
    if (s.language >= 0 && s.language_name.empty()) {
        int32_t slot = 0;
        for (const Bank& b : t.banks) if (b.code == s.language && b.code != 1) slot = int32_t(b.slot);
        t.languages.push_back(RawLanguage(t.banks, slot, "console code " + std::to_string(s.language)));
        t.selected = int32_t(t.languages.size()) - 1;
        t.how = "console code " + std::to_string(s.language) + " -> slot " + std::to_string(slot);
        return;
    }
    // 3 the name
    for (size_t i = 0; i < t.languages.size(); ++i)
        if (t.languages[i].name == t.requested) { t.selected = int32_t(i); t.how = "name"; return; }
    // 4 english, else the first
    for (size_t i = 0; i < t.languages.size(); ++i)
        if (t.languages[i].name == "english") { t.selected = int32_t(i); t.how = "no language '" + t.requested + "' on this disc: english"; return; }
    if (!t.languages.empty()) { t.selected = 0; t.how = "no language '" + t.requested + "' on this disc: the first"; }
}

}  // namespace

std::string RecognizeContent(const std::string& text, std::string* probe) {
    const std::vector<std::string> lines = Lines(text, 40);
    if (probe) *probe = lines.size() > 9 ? lines[9] : "";   // the "new game" line (the game's text 9)
    const char* best = nullptr;
    int best_score = 0;
    for (const Tell& tell : kTells) {
        int score = 0;
        for (const std::string& line : lines)
            for (size_t w = 0; w < tell.count; ++w)
                if (line == tell.words[w] || (std::strlen(tell.words[w]) >= 6 && line.rfind(tell.words[w], 0) == 0)) { ++score; break; }
        if (score > best_score) { best_score = score; best = tell.language; }
    }
    // Spanish's and English's first lines coincide (off/on/no/yes/back/continue are English in both... and so is the user's
    // patched disc's `Spanish` bank entirely): two or more tells decide
    return best && best_score >= 2 ? best : "";
}

bool EnumerateFromDisc(const std::string& iso_path, const std::vector<std::string>& slot_names_in, std::vector<Bank>& out,
                       std::string* error) {
    out.clear();
    std::vector<std::string> slot_names = slot_names_in;
    if (slot_names.empty()) slot_names.assign(kPalSlots, kPalSlots + sizeof kPalSlots / sizeof *kPalSlots);
    IsoFs iso;
    if (!iso.Open(iso_path, error)) return false;
    IsoFs::Entry bh, bd;
    if (!iso.Find("CRASH6/CRASH.BH", bh) || !iso.Find("CRASH6/CRASH.BD", bd) || bh.size < 8 || bh.size > (1u << 20)) {
        if (error) *error = "CRASH6/CRASH.BH or CRASH.BD not on the disc";
        return false;
    }
    std::vector<uint8_t> index(bh.size);
    if (iso.ReadBytes(uint64_t(bh.lba) * IsoFs::kSectorSize, bh.size, index.data()) != bh.size) {
        if (error) *error = "CRASH.BH did not read whole";
        return false;
    }
    // The index: Language\Code\<name>.txt -> (offset, size) in the BD
    struct Hit { uint32_t offset, size; };
    std::vector<std::pair<std::string, Hit>> code_banks;
    size_t at = 4;
    while (at + 4 <= index.size()) {
        uint32_t len = 0;
        std::memcpy(&len, &index[at], 4);
        at += 4;
        if (len > 512 || at + len + 8 > index.size()) break;
        std::string name(reinterpret_cast<const char*>(&index[at]), len);
        at += len;
        Hit h;
        std::memcpy(&h.offset, &index[at], 4);
        std::memcpy(&h.size, &index[at + 4], 4);
        at += 8;
        const std::string prefix = "Language\\Code\\";
        if (name.size() > prefix.size() + 4 && name.compare(0, prefix.size(), prefix) == 0 && name.compare(name.size() - 4, 4, ".txt") == 0)
            code_banks.emplace_back(name.substr(prefix.size(), name.size() - prefix.size() - 4), h);
    }
    for (uint32_t slot = 0; slot < slot_names.size(); ++slot) {
        const std::string& name = slot_names[slot];
        auto it = std::find_if(code_banks.begin(), code_banks.end(), [&](const auto& b) { return Lower(b.first) == Lower(name); });
        if (it == code_banks.end()) continue;
        Bank bank;
        bank.slot = slot;
        bank.name = name;
        bank.code = CodeOfSlotName(name);
        const size_t want = std::min<size_t>(kProbeBytes, it->second.size);
        std::string text(want, '\0');
        const size_t got = iso.ReadBytes(uint64_t(bd.lba) * IsoFs::kSectorSize + it->second.offset, want, reinterpret_cast<uint8_t*>(&text[0]));
        text.resize(got);
        const std::string content = RecognizeContent(text, &bank.probe);
        bank.recognised = !content.empty();
        bank.content = bank.recognised ? content : Lower(name);
        // The speech bank this index streams: Crash6\<name>.mb (SetVoiceBank(Crash6\<slot>) -> LoadSoundBank: <name>.mh + .mb).
        // Retail names it truthfully (nothing in a text patch touches it): its language is the slot's name.
        IsoFs::Entry mb;
        bank.speech = iso.Find("CRASH6/" + name + ".MB", mb) && !mb.directory && mb.size > 0;
        bank.speech_language = LanguageOfSlotName(name);
        out.push_back(bank);
    }
    if (out.empty() && error) *error = "no Language\\Code\\<slot>.txt bank in CRASH.BH for any slot the executable names";
    return !out.empty();
}

std::vector<Language> LanguagesOf(const std::vector<Bank>& banks) {
    std::vector<Language> out;
    const bool any_speech = std::any_of(banks.begin(), banks.end(), [](const Bank& b) { return b.speech; });
    for (const Bank& text : banks) {
        // one language per text content (a disc carrying the same text twice: the first slot's)
        if (std::any_of(out.begin(), out.end(), [&](const Language& l) { return l.name == text.content; })) continue;
        Language l;
        l.name = text.content;
        l.text_slot = int32_t(text.slot);
        const Bank* voice = nullptr;
        if (any_speech) {
            // the slot NAMED for the language with a speech bank, else English's, else the text slot
            for (const Bank& b : banks) if (b.speech && b.speech_language == text.content) { voice = &b; break; }
            if (!voice) for (const Bank& b : banks) if (b.speech && b.speech_language == "english") { voice = &b; break; }
        }
        if (!voice) voice = &text;
        l.voice_slot = int32_t(voice->slot);
        l.voice_language = voice->speech_language;
        l.text_language = l.name;
        l.label = l.name + (l.voice_language != l.name ? " (" + l.voice_language + " voice)" : "");
        out.push_back(l);
    }
    // english first, the rest in text-slot order
    std::stable_partition(out.begin(), out.end(), [](const Language& l) { return l.name == "english"; });
    // A language the disc has SPEECH in but no text (a text patch overwrote its slot's text with another language's): offered
    // too -- its own retail index (speech bank, FMV track, level voices, title cards, credits) with English's text slot (else
    // the slot's own text), the row saying which text it reads
    int32_t english_text_slot = -1;
    for (const Language& l : out) if (l.name == "english") english_text_slot = l.text_slot;
    for (const Bank& speech : banks) {
        if (!speech.speech) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Language& l) { return l.name == speech.speech_language; })) continue;
        Language l;
        l.name = speech.speech_language;
        l.voice_slot = int32_t(speech.slot);
        l.voice_language = speech.speech_language;
        l.text_slot = english_text_slot >= 0 ? english_text_slot : int32_t(speech.slot);
        l.text_language = english_text_slot >= 0 ? "english" : speech.content;
        l.label = l.name + " (" + l.text_language + " text)";
        out.push_back(l);
    }
    return out;
}

const Table& Resolve() {
    if (g_resolved) return g_table;
    g_resolved = true;
    g_table = Table{};
    const Settings& s = GetSettings();
    if (s.iso_path.empty()) {
        g_table.error = "no disc image";
    } else {
        std::vector<std::string> names = SlotNamesFromImage();
        if (names.empty()) names.assign(kPalSlots, kPalSlots + sizeof kPalSlots / sizeof *kPalSlots);
        std::string error;
        if (!EnumerateFromDisc(s.iso_path, names, g_table.banks, &error)) g_table.error = error;
    }
    g_table.languages = LanguagesOf(g_table.banks);
    Select(g_table);
    return g_table;
}

void Reset() {
    g_resolved = false;
    g_table = Table{};
}

namespace {
const Language* Current() {
    const Table& t = Resolve();
    return t.selected >= 0 && size_t(t.selected) < t.languages.size() ? &t.languages[size_t(t.selected)] : nullptr;
}
}  // namespace

int32_t ConsoleLanguage() {
    if (const Language* l = Current()) {
        const Bank* b = BankOfSlot(Resolve().banks, l->voice_slot);
        return b ? b->code : 1;
    }
    // No banks to look at (no disc / the tests' fake image): a raw code stands, anything else is English
    const int32_t raw = GetSettings().language;
    return raw >= 0 && raw <= 7 ? raw : 1;
}

int32_t TextBank() {
    const Language* l = Current();
    return l ? l->text_slot : -1;
}

uint32_t Count() {
    const Table& t = Resolve();
    uint32_t n = 0;
    for (const Language& l : t.languages) if (!l.raw) ++n;
    return n;
}

int32_t Selected() {
    const Table& t = Resolve();
    return t.selected;
}

std::string Label(uint32_t language) {
    const Table& t = Resolve();
    return language < t.languages.size() ? t.languages[language].label : "";
}

bool Select(uint32_t index) {
    Table& t = const_cast<Table&>(Resolve());
    if (index >= t.languages.size()) {
        std::fprintf(stderr, "[twinsanityevolved] language: the Options menu named language %u, which this disc has no row for\n", index);
        return false;
    }
    t.selected = int32_t(index);
    t.how = "options menu";
    const Language& l = t.languages[index];
    Settings s = GetSettings();
    s.language_bank = -1;
    s.language = -1;
    s.language_name = l.name;
    SetSettings(s);
    t.requested = l.name;
    if (!s.settings_path.empty()) {
        std::string err;
        if (!ps2r::SettingsFile::Set(s.settings_path, "game.language", l.name, &err))
            std::fprintf(stderr, "[twinsanityevolved] language: %s not written: %s\n", s.settings_path.c_str(), err.c_str());
        else
            std::fprintf(stderr, "[twinsanityevolved] language: game.language = %s written to %s\n", l.name.c_str(), s.settings_path.c_str());
    }
    std::fprintf(stderr, "[twinsanityevolved] %s\n", Describe().c_str());
    return true;
}

std::string Describe() {
    const Table& t = Resolve();
    std::string out = "language: ";
    if (t.banks.empty()) return out + "no banks enumerated (" + t.error + "); Language() answers " + std::to_string(ConsoleLanguage());
    out += "disc slots";
    for (const Bank& b : t.banks) {
        out += " [" + std::to_string(b.slot) + " " + b.name + ": text " + b.content + (b.recognised ? "" : " (slot name)") +
               " '" + b.probe + "', speech " + (b.speech ? b.speech_language + " (Crash6\\" + b.name + ".mb)" : "none") + "]";
    }
    out += "; languages";
    for (const Language& l : t.languages) {
        if (l.raw) continue;
        out += " [" + l.name + ": text slot " + std::to_string(l.text_slot) + " (" + l.text_language + "), voice slot " +
               std::to_string(l.voice_slot) + " (" + l.voice_language + "), row '" + l.label + "']";
    }
    out += "; requested '" + t.requested + "'";
    if (const Language* l = Current()) {
        out += " -> " + l->label + " by " + t.how + ": text slot " + std::to_string(l->text_slot) + ", voice slot " +
               std::to_string(l->voice_slot) + ", Language() " + std::to_string(ConsoleLanguage());
    }
    return out;
}

}  // namespace te::language
