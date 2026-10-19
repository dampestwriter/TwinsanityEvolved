// padlog.cpp -- see padlog.h.
#include "padlog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "te_language.h"
#include "te_services.h"

namespace te::padlog {

namespace {
const char* const kNames[16] = {"SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"};

struct Entry {
    uint64_t from, to;   // [from, to]; to = ~0 for "until the next entry"
    Pads::Sample sample;
};

struct Timeline {
    int32_t language_bank = -1;   // `# language-bank=N`: the raw slot (text = voice = N) the log was recorded against
    std::string language;         // `# language=<name>`: the language (tuple) the log was recorded against
    std::vector<Entry> entries;   // sorted by from
    Pads::Sample At(uint64_t boundary) const {
        Pads::Sample s;
        s.connected = true;
        // holds (closed ranges) are applied over the plain timeline: the last matching entry wins for plain lines,
        // holds OR their buttons in
        bool plain_found = false;
        Pads::Sample plain;
        plain.connected = true;
        uint16_t held = 0;
        bool have_hold = false;
        Pads::Sample hold;
        for (const Entry& e : entries) {
            if (boundary < e.from) break;
            if (e.to == ~0ull) { plain = e.sample; plain_found = true; }
            else if (boundary <= e.to) { held |= e.sample.buttons; hold = e.sample; have_hold = true; }
        }
        if (plain_found) s = plain;
        if (have_hold) {
            s.buttons |= held;
            if (hold.lx != 128 || hold.ly != 128 || hold.rx != 128 || hold.ry != 128) { s.lx = hold.lx; s.ly = hold.ly; s.rx = hold.rx; s.ry = hold.ry; }
        }
        s.connected = true;
        return s;
    }
};

Timeline g_play;
bool g_playing = false;
FILE* g_record = nullptr;
bool g_have_last = false;
Pads::Sample g_last;
uint64_t g_last_boundary = 0;

bool ParseSticks(std::istringstream& in, Pads::Sample& s) {
    int lx = 128, ly = 128, rx = 128, ry = 128;
    if (in >> lx) {
        if (!(in >> ly >> rx >> ry)) return false;
    }
    auto clamp = [](int v) { return uint8_t(std::min(255, std::max(0, v))); };
    s.lx = clamp(lx); s.ly = clamp(ly); s.rx = clamp(rx); s.ry = clamp(ry);
    return true;
}

bool Parse(const std::string& path, bool allow_hold, Timeline& out, std::string* error) {
    std::ifstream f(path);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::string line;
    size_t n = 0;
    out.entries.clear();
    while (std::getline(f, line)) {
        ++n;
        // The header `# language-bank=N`: the game's text bank the log was recorded against. The disc's text files
        // differ in size per bank and the drive model delivers them over time, so a recording's boundaries hold only
        // under its bank; the replay pins it unless the run named a language itself.
        if (line.rfind("# language-bank=", 0) == 0) out.language_bank = std::atoi(line.c_str() + 16);
        // `# language=<name>`: a recording under a language tuple (text slot / voice slot per the disc); the replay asks
        // for the same name
        if (line.rfind("# language=", 0) == 0) { out.language = line.substr(11); while (!out.language.empty() && std::isspace(static_cast<unsigned char>(out.language.back()))) out.language.pop_back(); }
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream in(line);
        std::string first;
        if (!(in >> first)) continue;
        Entry e{};
        std::string buttons;
        if (first == "hold") {
            if (!allow_hold) { if (error) *error = path + ":" + std::to_string(n) + ": hold lines are for scripts (--pad-script)"; return false; }
            if (!(in >> e.from >> e.to >> buttons)) { if (error) *error = path + ":" + std::to_string(n) + ": hold FROM TO BUTTONS"; return false; }
        } else {
            e.from = std::strtoull(first.c_str(), nullptr, 0);
            e.to = ~0ull;
            if (!(in >> buttons)) { if (error) *error = path + ":" + std::to_string(n) + ": BOUNDARY BUTTONS [lx ly rx ry]"; return false; }
        }
        if (!ParseButtons(buttons, e.sample.buttons)) { if (error) *error = path + ":" + std::to_string(n) + ": unknown button in '" + buttons + "'"; return false; }
        if (!ParseSticks(in, e.sample)) { if (error) *error = path + ":" + std::to_string(n) + ": sticks are lx ly rx ry"; return false; }
        e.sample.connected = true;
        out.entries.push_back(e);
    }
    std::stable_sort(out.entries.begin(), out.entries.end(), [](const Entry& a, const Entry& b) { return a.from < b.from; });
    return true;
}

bool Install(Timeline&& timeline) {
    g_play = std::move(timeline);
    g_playing = true;
    if (g_play.language_bank >= 0 || !g_play.language.empty()) {
        const Settings& was = GetSettings();
        const std::string what = g_play.language_bank >= 0 ? "raw bank " + std::to_string(g_play.language_bank) : "language '" + g_play.language + "'";
        if (was.language_bank < 0 && was.language < 0 && was.language_name.empty()) {
            Settings s = was;
            if (g_play.language_bank >= 0) s.language_bank = g_play.language_bank;
            else s.language_name = g_play.language;
            SetSettings(s);
            language::Reset();
            std::fprintf(stderr, "[twinsanityevolved] padlog: recorded against %s -- %s\n", what.c_str(), language::Describe().c_str());
        } else {
            std::fprintf(stderr, "[twinsanityevolved] padlog: recorded against %s, the run named its own language (the recording's boundaries may not hold)\n", what.c_str());
        }
    }
    Services::Get().pads().SetOverrideSampler([](unsigned port, unsigned slot) {
        if (port != 0 || slot != 0) { Pads::Sample none; none.connected = false; return none; }
        return g_play.At(Services::Get().clock().BoundaryCount());
    });
    return true;
}
}  // namespace

bool ParseButtons(const std::string& text, uint16_t& buttons) {
    buttons = 0;
    if (text.empty()) return false;
    if (std::isxdigit(static_cast<unsigned char>(text[0])) && text.find_first_not_of("0123456789abcdefABCDEFx") == std::string::npos) {
        buttons = static_cast<uint16_t>(std::strtoul(text.c_str(), nullptr, 16));
        return true;
    }
    if (text == "NONE" || text == "none") return true;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find('+', start);
        if (plus == std::string::npos) plus = text.size();
        const std::string name = text.substr(start, plus - start);
        bool found = false;
        for (int b = 0; b < 16; ++b) {
            if (name == kNames[b]) { buttons |= uint16_t(1u << b); found = true; break; }
        }
        if (!found) return false;
        start = plus + 1;
    }
    return true;
}

std::string ButtonNames(uint16_t buttons) {
    if (!buttons) return "NONE";
    std::string out;
    for (int b = 0; b < 16; ++b) {
        if (!(buttons & (1u << b))) continue;
        if (!out.empty()) out += '+';
        out += kNames[b];
    }
    return out;
}

bool Play(const std::string& path, std::string* error) {
    Timeline t;
    if (!Parse(path, false, t, error)) return false;
    std::fprintf(stderr, "[twinsanityevolved] padlog: playing %s (%zu entries)\n", path.c_str(), t.entries.size());
    return Install(std::move(t));
}

bool Script(const std::string& path, std::string* error) {
    Timeline t;
    if (!Parse(path, true, t, error)) return false;
    std::fprintf(stderr, "[twinsanityevolved] padlog: script %s (%zu entries)\n", path.c_str(), t.entries.size());
    return Install(std::move(t));
}

bool Record(const std::string& path, std::string* error) {
    Finish();
    g_record = std::fopen(path.c_str(), "w");
    if (!g_record) { if (error) *error = "cannot write " + path; return false; }
    std::fprintf(g_record, "# twinsanityevolved padlog v1 -- pad 0 per frame boundary: BOUNDARY BUTTONS(wire order, hex) LX LY RX RY  # names\n");
    {
        // The language this recording runs under -- a raw slot as `language-bank=N`, a tuple by its name
        const language::Table& t = language::Resolve();
        if (t.selected >= 0 && size_t(t.selected) < t.languages.size()) {
            const language::Language& l = t.languages[size_t(t.selected)];
            if (l.raw) std::fprintf(g_record, "# language-bank=%d\n", l.text_slot);
            else std::fprintf(g_record, "# language=%s\n", l.name.c_str());
        }
    }
    g_have_last = false;
    // A tap on whatever samples (the SDL sampler, a played log, the neutral pad): pad 0 as the game sees it
    Services::Get().pads().SetSampleTap([](unsigned port, unsigned slot, const Pads::Sample& s) {
        if (port == 0 && slot == 0 && g_record) {
            const uint64_t boundary = Services::Get().clock().BoundaryCount();
            const bool changed = !g_have_last || s.buttons != g_last.buttons || s.lx != g_last.lx || s.ly != g_last.ly ||
                                 s.rx != g_last.rx || s.ry != g_last.ry || s.connected != g_last.connected;
            if (changed) {
                std::fprintf(g_record, "%llu %04x %u %u %u %u  # %s\n", (unsigned long long)boundary, s.buttons, s.lx, s.ly, s.rx, s.ry,
                             ButtonNames(s.buttons).c_str());
                g_have_last = true;
                g_last = s;
            }
            g_last_boundary = boundary;
        }
    });
    std::fprintf(stderr, "[twinsanityevolved] padlog: recording to %s\n", path.c_str());
    return true;
}

void Note(const std::string& text) {
    if (!g_record) return;
    std::fprintf(g_record, "# %s\n", text.c_str());
    std::fflush(g_record);
}

bool Recording() { return g_record != nullptr; }

void Finish() {
    if (!g_record) return;
    std::fprintf(g_record, "# end at boundary %llu\n", (unsigned long long)g_last_boundary);
    std::fclose(g_record);
    g_record = nullptr;
}

}  // namespace te::padlog
