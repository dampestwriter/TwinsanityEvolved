// platform/nativeee (host side) -- the SPEEDRUN MODE mod: ONE real-time (RTA) timer, mm:ss.cc, drawn with the game's font
// (through the platform texts) horizontally centred at the top of the safe area -- nothing else on screen. The in-game time
// (IGT) is still kept and the splits on every level entry / exit are written into the pad-log recording as `# split` lines
// (a recorded run carries its own timer and its splits: verifiable), practice save states on a ring of slots with their own
// keys (F9 save, F10 load the newest, F11 the one before; the engine's F5 / F8 are not these), and F12 = reset to run start.
//
// Clocks. RTA runs from the run's start on the host's wall clock in a paced run and on the frame boundaries (1/field_hz) in a
// deterministic one -- it never stops (a pause is part of a run). IGT counts the frame boundaries the game spends PLAYING or
// WATCHING (the pause menu, a level load, the title and the saving dialogs do not count), so it is pause-aware by definition.
// The run starts at NEW GAME (GameController::StateNewGame) or, when no run is on, at the first level entered (a warp from
// the level select counts); F12 resets both timers and the splits and the run starts again at the next of those. Before the
// run starts the timer reads 00:00.00.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "padlog.h"
#include "savestate.h"
#include "te_host.h"
#include "te_input.h"
#include "te_mods.h"
#include "te_services.h"
#include "te_texts.h"

namespace te::mods {

namespace {

constexpr uint32_t kSlots = 8;
const char* const kKeySave = "F9", *const kKeyLoad = "F10", *const kKeyOlder = "F11", *const kKeyReset = "F12";
// Where the timer sits: horizontally centred, at the top of the safe area (the frame's outer 5 % is the title-safe margin a
// console game keeps clear; the fps counter's corner is 0.04), half the menu's glyph size, the game's menu white.
constexpr float kTimerX = 0.5f, kTimerY = 0.05f, kTimerScale = 0.5f;
constexpr uint32_t kTimerColour = 0x80C0C0C0u;

struct Split { uint64_t boundary; double rta, igt; std::string what, chunk; };

struct State {
    bool enabled = false;
    bool running = false;
    std::chrono::steady_clock::time_point rta_start;
    uint64_t start_boundary = 0;
    uint64_t igt_frames = 0;
    std::vector<Split> splits;
    unsigned last_state = 0;
    // the practice ring
    uint32_t next_slot = 0;        // the slot the next save takes
    uint32_t saved_slots = 0;      // how many slots hold a state (<= kSlots)
    uint32_t load_cursor = 0;      // F11 walks back from the newest
    bool held_save = false, held_load = false, held_older = false, held_reset = false;
    int text_source = -1;
};
State g;

double Rta() {
    if (!g.running) return 0.0;
    const Settings& s = GetSettings();
    if (s.deterministic) return double(Services::Get().clock().BoundaryCount() - g.start_boundary) / double(s.field_hz ? s.field_hz : 60u);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g.rta_start).count();
}

double Igt() {
    const Settings& s = GetSettings();
    return double(g.igt_frames) / double(s.field_hz ? s.field_hz : 60u);
}

// mm:ss.cc -- the minutes run on past 59 (an hour-long run reads 75:12.34), always two digits at least
std::string Stamp(double seconds) {
    if (seconds < 0) seconds = 0;
    const unsigned total = unsigned(seconds);
    const unsigned m = total / 60, sec = total % 60, cs = unsigned((seconds - double(total)) * 100.0) % 100;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02u:%02u.%02u", m, sec, cs);
    return buf;
}

void StartRun(uint64_t boundary, const char* why) {
    g.running = true;
    g.rta_start = std::chrono::steady_clock::now();
    g.start_boundary = boundary;
    g.igt_frames = 0;
    g.splits.clear();
    padlog::Note("speedrun run-start boundary=" + std::to_string(boundary) + " why=" + why);
    std::fprintf(stderr, "[twinsanityevolved] speedrun: run started @%llu (%s)\n", (unsigned long long)boundary, why);
}

void ResetRun(uint64_t boundary) {
    g.running = false;
    g.igt_frames = 0;
    g.splits.clear();
    padlog::Note("speedrun reset boundary=" + std::to_string(boundary));
    std::fprintf(stderr, "[twinsanityevolved] speedrun: reset to run start @%llu (the run starts again at NEW GAME or the next level entered)\n",
                 (unsigned long long)boundary);
}

// The title's world is a chunk with a character in it (the menu's beach): a chunk change seen while the game is at the title or
// its menus is not a level (states 0..8 lead to the title; RestartingTitle 20 leaves play)
bool InPlay(unsigned state) { return state >= 9 && state != 20 && state != 19; }

void RecordSplit(const FrameContext& ctx, const char* what, const std::string& chunk) {
    if (!InPlay(ctx.state)) return;
    if (!g.running) {
        if (std::string(what) == "enter") StartRun(ctx.boundary, "level-entered");
        else return;
    }
    Split s{ctx.boundary, Rta(), Igt(), what, chunk};
    g.splits.push_back(s);
    char line[256];
    std::snprintf(line, sizeof line, "split %s %s rta=%.3f igt=%.3f boundary=%llu n=%zu", what, chunk.c_str(), s.rta, s.igt,
                  (unsigned long long)ctx.boundary, g.splits.size());
    padlog::Note(line);
    std::fprintf(stderr, "[twinsanityevolved] speedrun: %s\n", line);
}

std::string PracticeDir() {
    const std::string& mc = GetSettings().mc_dir;
    const size_t slash = mc.find_last_of("/\\");   // either separator: a Windows path has backslashes
    const std::string base = (!mc.empty() && slash != std::string::npos) ? mc.substr(0, slash) : std::string(".");
    return base + "/practice";
}

std::string SlotPath(uint32_t slot) { return PracticeDir() + "/speedrun_slot" + std::to_string(slot) + ".tes"; }

void Keys(const FrameContext& ctx) {
    auto edge = [](const char* key, bool& held) {
        const bool now = input::KeyHeld(key);
        const bool pressed = now && !held;
        held = now;
        return pressed;
    };
    if (edge(kKeySave, g.held_save)) {
        const std::string path = SlotPath(g.next_slot);
        std::error_code ec;
        std::filesystem::create_directories(PracticeDir(), ec);
        savestate::RequestSave(path);
        padlog::Note("speedrun practice-save slot=" + std::to_string(g.next_slot) + " boundary=" + std::to_string(ctx.boundary));
        std::fprintf(stderr, "[twinsanityevolved] speedrun: practice state -> slot %u (%s) @%llu\n", g.next_slot, path.c_str(), (unsigned long long)ctx.boundary);
        g.load_cursor = g.next_slot;
        g.next_slot = (g.next_slot + 1) % kSlots;
        if (g.saved_slots < kSlots) g.saved_slots++;
    }
    if (edge(kKeyLoad, g.held_load) || edge(kKeyOlder, g.held_older)) {
        if (g.saved_slots == 0) {
            std::fprintf(stderr, "[twinsanityevolved] speedrun: no practice state saved yet (%s saves one)\n", kKeySave);
        } else {
            const bool older = g.held_older && !g.held_load;
            if (older) g.load_cursor = (g.load_cursor + kSlots - 1) % kSlots;
            else g.load_cursor = (g.next_slot + kSlots - 1) % kSlots;   // the newest
            const std::string path = SlotPath(g.load_cursor);
            savestate::RequestLoad(path);
            padlog::Note("speedrun practice-load slot=" + std::to_string(g.load_cursor) + " boundary=" + std::to_string(ctx.boundary));
            std::fprintf(stderr, "[twinsanityevolved] speedrun: practice state <- slot %u (%s) @%llu\n", g.load_cursor, path.c_str(), (unsigned long long)ctx.boundary);
        }
    }
    if (edge(kKeyReset, g.held_reset)) ResetRun(ctx.boundary);
}

void OnFrame(const FrameContext& ctx) {
    // the run starts at NEW GAME
    if (ctx.state == 10 && g.last_state != 10) StartRun(ctx.boundary, "new-game");
    g.last_state = ctx.state;
    if (g.running && ctx.playing) g.igt_frames++;
    Keys(ctx);
}

// The one thing on screen: the RTA timer, centred at the top of the safe area. IGT, the splits and the run's state live in
// the recording and the log, not in the frame.
void Texts(std::vector<texts::Line>& out) {
    if (!g.enabled) return;
    texts::Line rta;
    rta.x = kTimerX; rta.y = kTimerY; rta.scale = kTimerScale; rta.colour = kTimerColour; rta.alignment = texts::Top | texts::Centre;
    rta.text = Stamp(Rta());
    out.push_back(rta);
}

}  // namespace

void RegisterSpeedrun() {
    Mod m;
    m.id = "speedrun";
    m.name = "speedrun mode";
    m.version = "1.0.0";
    m.page_title = "speedrun";
    m.page_rows = [] { return 1u; };
    m.page_row_label = [](uint32_t row) { return std::string(row == 0 ? "reset to run start" : ""); };
    m.page_row_chosen = [](uint32_t row, WarpTarget&) {
        if (row == 0) ResetRun(Services::Get().clock().BoundaryCount());
        return ActionNone;
    };
    m.on_enable = [](bool on) {
        g.enabled = on;
        if (on && g.text_source < 0) g.text_source = texts::Register("speedrun", Texts);
        if (!on && g.text_source >= 0) { texts::Unregister(g.text_source); g.text_source = -1; }
        if (on) std::fprintf(stderr, "[twinsanityevolved] speedrun: on -- %s saves a practice state (ring of %u), %s loads the newest, %s the one before, %s resets to run start; splits into the pad log\n",
                             kKeySave, kSlots, kKeyLoad, kKeyOlder, kKeyReset);
    };
    m.on_frame = OnFrame;
    m.on_level_enter = [](const FrameContext& ctx, const std::string& chunk) { RecordSplit(ctx, "enter", chunk); };
    m.on_level_exit = [](const FrameContext& ctx, const std::string& chunk) { RecordSplit(ctx, "exit", chunk); };
    m.on_save = [](const std::string& save, const std::string& file) {
        if (g.running) padlog::Note("speedrun card-save " + save + "/" + file + " rta=" + Stamp(Rta()) + " igt=" + Stamp(Igt()));
    };
    Register(m);
}

}  // namespace te::mods
