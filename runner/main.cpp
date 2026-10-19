// twinsanityevolved -- the host runner.
//
// Constructs the engine's MainMemory, binds it as the game module's linear memory, instantiates the wasm2c module,
// loads the retail data image from the PAL executable on the disc image at RUN time, patches retail code pointers to
// wasm table indices, and calls the module's `entry`. Seam imports without an owner body yet hit the generated weak
// stubs (seam_stubs.cpp), which log once; the module's only imports are env.memory and the seam.
//
//   twinsanityevolved --iso <disc image> [--elf-in-iso SLES_525.68] [--dry-run] [--verbose]
//   twinsanityevolved --elf <SLES_525.68 extracted> [--dry-run]
//
// --dry-run: instantiate, load, patch, report, exit 0 (the toolchain's own smoke test). Game data is read at run time
// only (charter): nothing is extracted to disk.
//
// The game runs on its own fiber (te_game.h) and yields to this host loop at save-state checkpoints (the end of every
// frame boundary when a save is due), at Platform::System::Exit and when entry returns or traps. Save states:
// --save-state PATH@N writes PATH when boundary N has passed (repeatable); --load-state PATH restores before the first
// switch into the fiber, so the loaded game continues at its saved boundary. Both re-exec the runner without ASLR.
#include <csignal>
#if defined(_WIN32) || defined(__vita__)
#include <chrono>
#include <thread>
#endif
#if !defined(_WIN32)
#include <unistd.h>   // _exit (the watchdog's second firing)
#endif
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/ee/mem/main_memory.h"
#include "engine/core/config/config_loader.h"
#include "engine/core/config/settings_file.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "te_graphics_hooks.h"
#include "engine/platform/host/platform.h"
#include "padlog.h"
#include "savestate.h"
#include "te_audio.h"
#include "te_env.h"
#include "te_game.h"
#include "te_harness.h"
#include "te_host.h"
#include "te_display.h"
#include "te_input.h"
#include "te_language.h"
#include "te_mods.h"
#include "te_autopilot.h"
#include "te_probe.h"
#include "te_savestate.h"
#if defined(__vita__)
#include "te_vita.h"   // the Vita lane: baked arguments, the log file, the memory report
#endif
#include "te_services.h"
#include "te_texts.h"
#include "te_run.h"
#include "wasm-rt.h"

extern "C" const uint32_t te_known_function_starts[];
extern "C" const uint32_t te_known_function_start_count;
extern w2c_env* te_diag_env;   // runner/te_env.cpp: the call_indirect adapter log reads the retail table through it

namespace {
w2c_env g_env;
w2c_game g_instance{};   // a global: its address is part of the game fiber's frames (save states)
int g_exit_status = 0;
volatile sig_atomic_t g_watchdog = 0;

// Pending --save-state requests: (path, boundary); taken at the first checkpoint at or after that boundary
struct SaveRequest { std::string path; uint64_t at; bool done; };
std::vector<SaveRequest> g_save_requests;
std::string g_save_now;   // the path the host writes when the fiber yields with Reason::Checkpoint
std::string g_load_now;   // the path the host restores when the fiber yields with Reason::LoadState

void Usage() {
    std::fprintf(stderr, "usage: twinsanityevolved (--iso <image> [--elf-in-iso NAME] | --elf <file>) [--dry-run] [--verbose] [--run-seconds N] [--test-trap] [--trace-state]\n"
                         "       [--deterministic] [--hz 60|50] [--language <name>|0..7] [--language-bank N] [--config ps2r.ini|none] [--aspect auto|4:3|16:9|16:10|21:9] [--frame-rate 30|60|120|144|240|uncapped] [--fps-counter on|off] [--mod ID ...] [--mc-dir DIR] [--no-sdl-input] [--no-kbm] [--press 0xBUTTONS] [--no-audio] [--skip-movies] [--stream-instant]\n"
                         "       [--save-state PATH@BOUNDARY ...] [--load-state PATH] [--stop-at BOUNDARY] [--present-ct16]\n"
                         "       [--record-pad FILE] [--play-pad FILE] [--pad-script FILE] [--inject-key KEY@FROM-TO ...] [--close-at BOUNDARY]\n"
                         "       [--harness-out DIR]\n"
                         "       [--trace-player N] [--dump-collision DIR] [--autopilot PLAN]\n");
}

// The checkpoint (te::savestate::Checkpoint, the last statement of te::Clock::Boundary) on the game fiber: yield to
// the host when a save is due; the host writes it and resumes. Nothing else happens here.
void OnCheckpoint(uint64_t boundary) {
    for (SaveRequest& r : g_save_requests) {
        if (r.done || boundary < r.at) continue;
        r.done = true;
        g_save_now = r.path;
        te::game::Yield(te::game::Reason::Checkpoint, 0, boundary);
    }
    // The paths go straight into the globals: NO heap-owning local may be live across a Yield here -- the saved stack carries
    // this frame, and a later load (in the same process) resumes it with that local's heap pointer long freed (a
    // std::string local here aborted the third practice load with "pointer being freed was not allocated").
    if (te::savestate::TakeSaveRequest(g_save_now)) te::game::Yield(te::game::Reason::Checkpoint, 0, boundary);
    if (te::savestate::TakeLoadRequest(g_load_now)) te::game::Yield(te::game::Reason::LoadState, 0, boundary);   // restored in place
    if (g_watchdog) te::game::Yield(te::game::Reason::Exit, 0, boundary);
}
}  // namespace

namespace te {
uint8_t* GuestMemory() { return g_env.memory.data; }
std::size_t GuestMemorySize() { return g_env.memory.size; }
const char* GuestString(uint32_t address, std::size_t max) {
    const uint8_t* p = GuestPointer(address, 1);
    if (!p) return nullptr;
    const std::size_t room = std::min<std::size_t>(max, GuestMemorySize() - address);
    return std::memchr(p, 0, room) ? reinterpret_cast<const char*>(p) : nullptr;
}
[[noreturn]] void RequestExit(int status) {
    g_exit_status = status;
    te::game::ExitFromGame(status);   // returns only when not on the game fiber
    std::exit(status);
}
}  // namespace te

int main(int argc, char** argv) {
#if defined(__vita__)
    te::vita::Startup(&argc, &argv);
#endif
    std::string iso, elf_path, elf_in_iso = "SLES_525.68";
    bool dry_run = false, verbose = false, test_trap = false, no_audio = false, trace_state = false;
    unsigned trace_player = 0;
    unsigned run_seconds = 0;
    te::Settings settings;   // the services' run settings (platform/nativeee/host/te_host.h)
    uint32_t press = 0;
    std::string load_state, record_pad, play_pad, pad_script, harness_out, autopilot, dump_collision, config_path, aspect_override, frame_rate_override;
    int fps_counter_override = -1;   // --fps-counter on|off for this run (the judge's arms); -1 = the file's / off
    std::vector<std::string> mods_on, mods_from_file;   // --mod <id> (repeatable, on for this run, not persisted); ps2r.ini's mods.<id>.enabled
    uint64_t stop_at = 0, close_at = 0;
#if defined(_WIN32)
    if (const char* home = std::getenv("USERPROFILE")) settings.mc_dir = std::string(home) + "/.twinsanityevolved/memcards";
#else
    if (const char* home = std::getenv("HOME")) settings.mc_dir = std::string(home) + "/.twinsanityevolved/memcards";
#endif
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 >= argc) { Usage(); std::exit(2); } out = argv[++i]; };
        if (a == "--iso") next(iso);
        else if (a == "--elf") next(elf_path);
        else if (a == "--elf-in-iso") next(elf_in_iso);
        else if (a == "--dry-run") dry_run = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--test-trap") test_trap = true;   // self-test: a wasm trap must unwind to main, not crash
        else if (a == "--run-seconds") { std::string v; next(v); run_seconds = unsigned(std::atoi(v.c_str())); }   // watchdog: exit 0 after N s
        else if (a == "--deterministic") settings.deterministic = true;        // no pacer: one field per boundary, fixed RTC
        else if (a == "--hz") { std::string v; next(v); settings.field_hz = unsigned(std::atoi(v.c_str())); }
        else if (a == "--language") {   // a language NAME (english, french, ...) resolved against the disc's banks by content, or a raw console code 0..7
            std::string v; next(v);
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) settings.language = std::atoi(v.c_str());
            else settings.language_name = v;
        }
        else if (a == "--language-bank") { std::string v; next(v); settings.language_bank = std::atoi(v.c_str()); }   // a slot of the game's language table (diagnostic)
        else if (a == "--config") next(config_path);
        else if (a == "--fps-counter") { std::string v; next(v); fps_counter_override = (v == "on" || v == "1") ? 1 : 0; }   // render fps + game Hz through the game's text system
        else if (a == "--mod") { std::string v; next(v); mods_on.push_back(v); }   // a built-in mod (level_select, speedrun) on for this run
        else if (a == "--aspect") { std::string v; next(v); aspect_override = v; }
        else if (a == "--frame-rate") { std::string v; next(v); frame_rate_override = v; }   // the presenter's cadence for this run (30|60|120|144|240|uncapped)   // the display shape for this run (auto|4:3|16:9|16:10|21:9; the judge's arm)   // the settings file read (and written by the Options menu); default <home>/.twinsanityevolved/ps2r.ini, `none` = no file
        else if (a == "--mc-dir") next(settings.mc_dir);
        else if (a == "--no-sdl-input") settings.sdl_input = false;
        else if (a == "--no-kbm") settings.kbm = false;
        else if (a == "--press") { std::string v; next(v); press = uint32_t(std::strtoul(v.c_str(), nullptr, 0)); }   // hold the game's button word on pad 0
        else if (a == "--no-audio") no_audio = true;   // mix without opening the SDL audio device (headless / CI)
        else if (a == "--skip-movies") settings.skip_movies = true;   // FEATURES: every movie skipped (the stub's behaviour)
        else if (a == "--stream-instant") settings.stream_instant = true;   // A/B: the pre-drive-model stream façade (reads complete in the call)
        else if (a == "--present-ct16") settings.present_ct16 = true;   // the retail look -- the dithered 16-bit present (TE_GFX_PRESENT_CT16=1 too)
        else if (a == "--trace-state") trace_state = true;   // log GameController::State() changes at the boundaries (title judge)
        else if (a == "--trace-player") { std::string v; next(v); trace_player = unsigned(std::atoi(v.c_str())); if (!trace_player) trace_player = 1; }   // authoring probe: position/chunk/state every N boundaries
        else if (a == "--save-state") {   // PATH@N: write PATH at the first checkpoint at or after boundary N (N omitted: the first)
            std::string v; next(v);
            SaveRequest r{v, 0, false};
            const size_t at = v.rfind('@');
            if (at != std::string::npos) { r.path = v.substr(0, at); r.at = std::strtoull(v.c_str() + at + 1, nullptr, 0); }
            g_save_requests.push_back(r);
        }
        else if (a == "--load-state") next(load_state);
        else if (a == "--stop-at") { std::string v; next(v); stop_at = std::strtoull(v.c_str(), nullptr, 0); }   // exit 0 at boundary N
        else if (a == "--record-pad") next(record_pad);    // write pad 0's samples per boundary (fixtures/padlogs format)
        else if (a == "--play-pad") next(play_pad);        // replace the sampler with a recorded padlog
        else if (a == "--pad-script") next(pad_script);    // a padlog with `hold` lines (authoring: boundary ranges)
        else if (a == "--harness-out") next(harness_out);   // the harness leg (te_harness.h): frames.fhash / audio.ahash / run.meta / state.trace / frames.pdesc
        else if (a == "--autopilot") next(autopilot);
        else if (a == "--dump-collision") next(dump_collision);   // with --trace-player: each chunk's collision triangles as OBJ into DIR (authoring)      // assisted play: a plan driven from the probe (record it with --record-pad)
        else if (a == "--close-at") { std::string v; next(v); close_at = std::strtoull(v.c_str(), nullptr, 0); }   // post the window's close (SDL_QUIT) at boundary N
        else if (a == "--inject-key") {   // KEY@FROM-TO: the SDL key held through the keyboard backend at those boundaries (menus by keyboard)
            std::string v; next(v);
            const size_t at = v.rfind('@'), dash = v.rfind('-');
            if (at == std::string::npos || dash == std::string::npos || dash < at) { Usage(); return 2; }
            std::string error;
            if (!te::input::InjectKey(v.substr(0, at), std::strtoull(v.c_str() + at + 1, nullptr, 0), std::strtoull(v.c_str() + dash + 1, nullptr, 0), &error)) {
                std::fprintf(stderr, "[twinsanityevolved] --inject-key: %s\n", error.c_str());
                return 2;
            }
        }
        else { Usage(); return 2; }
    }
    if (iso.empty() && elf_path.empty()) { Usage(); return 2; }
    if (!g_save_requests.empty() || !load_state.empty()) {
        std::string warning;
        te::savestate_runner::EnsureNoAslr(argc, argv, &warning);   // re-execs (and never returns) when ASLR moved us
        if (!warning.empty()) std::fprintf(stderr, "[twinsanityevolved] savestate: %s\n", warning.c_str());
    }
    settings.iso_path = iso;
    settings.verbose = verbose;
    // the settings file ps2r.ini (the engine's schema: ConfigLoader reads it, SettingsFile writes one key) beside
    // the memory cards, <home>/.twinsanityevolved/ps2r.ini, or --config PATH. The file supplies what the command line did not
    // (game.language today; the Display page's video.* keys). A deterministic run (the judges, CI) never writes the
    // file unless --config names one; a play run creates it with the defaults on first run (the "first-run page" of a port
    // without a launcher: the file the player edits, the Options menu writes).
    {
        const bool explicit_config = !config_path.empty() && config_path != "none";
        if (config_path.empty()) {
            const size_t slash = settings.mc_dir.rfind('/');
            if (!settings.mc_dir.empty() && slash != std::string::npos) config_path = settings.mc_dir.substr(0, slash) + "/ps2r.ini";
        }
        if (config_path == "none") config_path.clear();
        te::display::Settings display;   // the engine's video.* keys the port honours; the defaults when there is no file
        if (!config_path.empty()) {
            ps2r::Config cfg;
            cfg.video.vsync = false;   // the port's default (its pacer times the presents); the file's video.vsync overrides
            std::string err;
            bool existed = false;
            if (!ps2r::ConfigLoader::ApplyIniFile(config_path, cfg, &err, &existed)) {
                std::fprintf(stderr, "[twinsanityevolved] settings: %s not applied: %s\n", config_path.c_str(), err.c_str());
            } else if (existed) {
                if (settings.language_name.empty() && settings.language < 0 && settings.language_bank < 0) settings.language_name = cfg.game.language;
                display.aspect = int(cfg.video.aspect);
                display.fit = int(cfg.video.fit_mode);
                display.scale = cfg.video.scale > 0 ? cfg.video.scale : 1;
                display.texture_filter = int(cfg.video.texture_filter);
                display.vsync = cfg.video.vsync;
                display.fullscreen = int(cfg.video.fullscreen);
                display.offset_x = cfg.video.custom_offset_x;
                display.offset_y = cfg.video.custom_offset_y;
                {   // display.fps_counter / video.frame_rate -- read by name so the port still builds against an engine without the keys
                    std::string v;
                    if (cfg.Get("display.fps_counter", &v)) display.fps_counter = v == "on";
                    ps2r::ps2e::PresentPacer::Rate rate;
                    if (cfg.Get("video.frame_rate", &v) && ps2r::ps2e::PresentPacer::Parse(v.c_str(), &rate)) display.frame_rate = int(rate);
                    // mods.<id>.enabled for every built-in (the ids the framework knows; read by name). Registration only:
                    // the services are constructed after te::SetSettings (InstallBuiltins below adds the frame hook)
                    te::mods::RegisterBuiltins();
                    for (uint32_t i = 0; i < te::mods::Count(); ++i) {
                        const te::mods::Mod* m = te::mods::At(i);
                        if (cfg.Get("mods." + m->id + ".enabled", &v) && v == "on") mods_from_file.push_back(m->id);
                    }
                }
                if (verbose) std::fprintf(stderr, "[twinsanityevolved] settings: %s read (game.language = %s)\n", config_path.c_str(), cfg.game.language.c_str());
            } else if (!settings.deterministic || explicit_config) {
                // first run: the file with the defaults, so the player finds it
                const std::string language = settings.language_name.empty() ? "english" : settings.language_name;
                if (ps2r::SettingsFile::Set(config_path, "game.language", language, &err))
                    std::fprintf(stderr, "[twinsanityevolved] settings: %s created (game.language = %s)\n", config_path.c_str(), language.c_str());
                else
                    std::fprintf(stderr, "[twinsanityevolved] settings: %s not created: %s\n", config_path.c_str(), err.c_str());
            }
            if (!settings.deterministic || explicit_config) settings.settings_path = config_path;
        }
        // The engine's vsync default is ON (a Windows release blocker); the port's pacer times its presents, so the port's
        // default without a file is OFF (the engine's VideoConfig default is read only when the file exists and names it).
        if (!aspect_override.empty()) {
            static const char* const names[] = {"auto", "4:3", "16:9", "16:10", "21:9"};
            bool found = false;
            for (int i = 0; i < 5; ++i) if (aspect_override == names[i]) { display.aspect = i; found = true; }
            if (!found) { std::fprintf(stderr, "[twinsanityevolved] --aspect: auto|4:3|16:9|16:10|21:9\n"); return 2; }
        }
        if (fps_counter_override >= 0) display.fps_counter = fps_counter_override == 1;
        // --frame-rate for the run (the judges' arms), else TE_FRAME_RATE (a judge script that cannot add arguments)
        if (frame_rate_override.empty() && std::getenv("TE_FRAME_RATE")) frame_rate_override = std::getenv("TE_FRAME_RATE");
        if (!frame_rate_override.empty()) {
            ps2r::ps2e::PresentPacer::Rate rate;
            if (!ps2r::ps2e::PresentPacer::Parse(frame_rate_override.c_str(), &rate)) {
                std::fprintf(stderr, "[twinsanityevolved] --frame-rate: 30|60|120|144|240|uncapped\n");
                return 2;
            }
            display.frame_rate = int(rate);
        }
        te::display::Configure(display);
        te::texts::InstallFpsCounter();
    }
    // the speedrun mod's practice states load in the SAME process (no re-exec), but a state saved in one play session must
    // load in the next: the runner runs without ASLR whenever the mod is on at start-up (as it does for --save-state / --load-state)
    {
        bool speedrun = false;
        for (const std::string& id : mods_on) speedrun |= id == "speedrun";
        for (const std::string& id : mods_from_file) speedrun |= id == "speedrun";
        if (speedrun && g_save_requests.empty() && load_state.empty()) {
            std::string warning;
            te::savestate_runner::EnsureNoAslr(argc, argv, &warning);
            if (!warning.empty()) std::fprintf(stderr, "[twinsanityevolved] savestate: %s\n", warning.c_str());
        }
    }
    te::SetSettings(settings);
    // the built-in mods (all off unless ps2r.ini or --mod says so; the switches persist through the Options > Mods page)
    te::mods::InstallBuiltins();
    te::mods::Configure(mods_from_file);
    for (const std::string& id : mods_on) {
        bool known = false;
        for (uint32_t i = 0; i < te::mods::Count(); ++i) if (te::mods::At(i)->id == id) { te::mods::SetEnabled(i, true, false); known = true; }
        if (!known) { std::fprintf(stderr, "[twinsanityevolved] --mod %s: not a built-in mod (%s)\n", id.c_str(), te::mods::Describe().c_str()); return 2; }
    }
    std::fprintf(stderr, "[twinsanityevolved] %s\n", te::mods::Describe().c_str());
    te::audio::Configure(!no_audio);
    if (!harness_out.empty()) {
        char build_id[64];
        std::snprintf(build_id, sizeof build_id, "twinsanityevolved %08x", te::savestate_runner::BuildId());
        std::string error;
        if (!te::harness::Arm(harness_out, build_id, &error)) { std::fprintf(stderr, "[twinsanityevolved] --harness-out: %s\n", error.c_str()); return 2; }
        te::harness::Set("disc", iso.empty() ? elf_path : iso);
        te::harness::Set("elf", elf_in_iso);
        te::harness::Set("field_hz", std::to_string(settings.field_hz));
        te::harness::Set("skip_movies", settings.skip_movies ? "on" : "off");
        te::harness::Set("stream_model", settings.stream_instant ? "instant" : "drive");
        te::harness::Set("language_request", settings.language_name.empty() ? std::to_string(settings.language) : settings.language_name);
        te::harness::Set("play_pad", play_pad);
        te::harness::Set("pad_script", pad_script);
        te::harness::Set("load_state", load_state);
        std::fprintf(stderr, "[twinsanityevolved] harness leg: %s\n", harness_out.c_str());
    }

    // 1. the engine's memory IS the module's memory
    MainMemory ram;
    te::BindMainMemory(g_env, ram);

    // 2. the module
    wasm_rt_init();
    w2c_game& instance = g_instance;
    wasm2c_game_instantiate(&instance, &g_env);
    g_env.instance = &instance;
    te_diag_env = &g_env;
    std::fprintf(stderr, "[twinsanityevolved] module: memory %u MB bound to MainMemory, heap base 0x%08X, data end 0x%08X, retail image at 0x%08X, %u retail functions in the table\n",
                 unsigned(g_env.memory.size >> 20), *w2c_game_0x5F_heap_base(&instance), *w2c_game_0x5F_data_end(&instance),
                 *w2c_game_te_retail_image(&instance),
                 *reinterpret_cast<const uint32_t*>(g_env.memory.data + *w2c_game_te_retail_function_count(&instance)));

    // 3. the retail data image, from the disc at run time
    std::vector<uint8_t> elf;
    std::string error;
    if (!iso.empty()) {
        if (!te::ReadElfFromIso(iso, elf_in_iso, elf, &error)) { std::fprintf(stderr, "[twinsanityevolved] %s\n", error.c_str()); return 3; }
    } else {
        std::ifstream f(elf_path, std::ios::binary);
        if (!f) { std::fprintf(stderr, "[twinsanityevolved] cannot read %s\n", elf_path.c_str()); return 3; }
        elf.assign(std::istreambuf_iterator<char>(f), {});
    }
    te::RetailImage image;
    if (!te::LoadRetailImage(g_env, elf, image, &error)) { std::fprintf(stderr, "[twinsanityevolved] retail image: %s\n", error.c_str()); return 4; }

    // 4. retail code pointers -> table indices
    std::vector<uint32_t> starts(te_known_function_starts, te_known_function_starts + te_known_function_start_count);
    te::PatchReport rep = te::PatchRetailCodePointers(g_env, starts);
    std::fprintf(stderr, "[twinsanityevolved] retail image loaded (%zu-byte ELF); code pointers: %u words scanned, %u patched, %u unported bodies, %u stray in code range\n",
                 elf.size(), rep.words_scanned, rep.patched, rep.unported_hits, rep.stray_code_range);
    if (verbose) {
        for (auto& [where, fn] : rep.unported) std::fprintf(stderr, "  unported: word 0x%08X -> retail function 0x%08X\n", where, fn);
    }
    // 4a. The language setting: the disc's text banks, their contents and the selection (the retail image names the slots, the disc the banks)
    std::fprintf(stderr, "[twinsanityevolved] %s\n", te::language::Describe().c_str());
    // 4b. vtable slots whose body's type differs from the slot's declared type -> thunks of the declared type
    te::VtableThunkReport vt = te::PatchVtableThunks(g_env, verbose);
    std::fprintf(stderr, "[twinsanityevolved] vtable thunks: %u slots checked, %u matched, %u thunked, %u pure-virtual, %u beyond vtable end, %u unported, %u pool exhausted, %u duplicate rows\n",
                 vt.slots_checked, vt.matched, vt.thunked, vt.pure, vt.beyond_vtable, vt.unported, vt.pool_exhausted, vt.already);
    for (const std::string& s : vt.pool_exhausted_signatures) std::fprintf(stderr, "  POOL EXHAUSTED for %s -- raise TE_VT_POOL (CMake) and rebuild\n", s.c_str());
    if (dry_run) {
        std::fprintf(stderr, "[twinsanityevolved] --dry-run: not entering the game\n");
        wasm2c_game_free(&instance);
        return 0;
    }

    // 4c. the game fiber + the save-state sections + the checkpoint handler
    std::string fiber_error;
    if (!te::game::Prepare(&instance, test_trap, &fiber_error)) { std::fprintf(stderr, "[twinsanityevolved] %s\n", fiber_error.c_str()); return 5; }
    if (verbose || !g_save_requests.empty() || !load_state.empty()) {
        const te::game::Fiber& f = te::game::Get();
        const te::savestate_runner::ImageInfo img = te::savestate_runner::Image();
        std::fprintf(stderr, "[twinsanityevolved] fiber: game stack %p (%zu MB, %s), image 0x%llx+0x%llx (slide 0x%llx), memory %p, instance %p\n",
                     static_cast<void*>(f.stack), f.stack_size >> 20, f.at_hint ? "at the fixed address" : "NOT at the fixed address",
                     (unsigned long long)img.base, (unsigned long long)img.size, (unsigned long long)img.slide,
                     static_cast<void*>(g_env.memory.data), static_cast<void*>(&instance));
        if (verbose)
            std::fprintf(stderr, "[twinsanityevolved] fiber: env %p, env.memory %p, instance.memory %p, table data %p (%u entries)\n",
                         static_cast<void*>(&g_env), static_cast<void*>(&g_env.memory), static_cast<void*>(instance.w2c_env_memory),
                         static_cast<void*>(instance.w2c_0x5F_indirect_function_table.data), instance.w2c_0x5F_indirect_function_table.size);
    }
    te::savestate_runner::Register(g_env, instance);
    te::savestate::SetCheckpointHandler(OnCheckpoint);

    if (run_seconds) {
        // The watchdog: the game yields at its next checkpoint (OnCheckpoint sees the flag); a game that never reaches
        // a boundary again is left by the alarm's second firing.
#if defined(_WIN32) || defined(__vita__)
        // No SIGALRM (Win32; newlib on the Vita): a detached thread plays the alarm (the same two firings, 2 s apart).
        std::thread([run_seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
            g_watchdog = 1;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            _exit(0);
        }).detach();
#else
        std::signal(SIGALRM, [](int) {
            if (g_watchdog) _exit(0);
            g_watchdog = 1;
            alarm(2);
        });
        alarm(run_seconds);
#endif
    }
    if (press) te::Services::Get().pads().HoldTestButtons(press);
    if (stop_at) {
        te::Services::Get().clock().AddBoundaryHook("stop-at", [stop_at](uint64_t boundary) {
            if (boundary >= stop_at) { std::fprintf(stderr, "[twinsanityevolved] --stop-at %llu reached\n", (unsigned long long)boundary); g_watchdog = 1; }
        });
    }
    if (close_at) {
        te::Services::Get().clock().AddBoundaryHook("close-at", [close_at](uint64_t boundary) {
            static bool posted = false;
            if (posted || boundary < close_at) return;
            posted = true;
            std::fprintf(stderr, "[twinsanityevolved] --close-at %llu: window close posted (%s)\n", (unsigned long long)boundary,
                         te::input::RequestWindowClose() ? "SDL_QUIT queued" : "no SDL event queue");
        });
    }
    // The pad log (fixtures/padlogs): record pad 0's samples, play a recording back, or run an authoring script
    std::string pad_error;
    if (!play_pad.empty() && !te::padlog::Play(play_pad, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] padlog: %s\n", pad_error.c_str()); return 2; }
    if (!pad_script.empty() && !te::padlog::Script(pad_script, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] padlog: %s\n", pad_error.c_str()); return 2; }
    if (!autopilot.empty() && !te::autopilot::Load(autopilot, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] autopilot: %s\n", pad_error.c_str()); return 2; }
    if (!record_pad.empty() && !te::padlog::Record(record_pad, &pad_error)) { std::fprintf(stderr, "[twinsanityevolved] padlog: %s\n", pad_error.c_str()); return 2; }
    if (trace_player) te::probe::Install(trace_player, dump_collision);
    if (trace_state) {
        // The game controller's state word (te::harness::ReadGameState: include/game/gamecontroller.h GameControllerStates
        // through the retail global G_GameController) at every boundary: one line per change, so a run log records when
        // StateTitle (7) was reached and for how many boundaries it held. (A harness leg writes the same to state.trace.)
        te::Services::Get().clock().AddBoundaryHook("trace-state", [](uint64_t boundary) {
            static uint64_t last = ~0ull;
            unsigned state = 0, lastState = 0, nextState = 0;
            if (!te::harness::ReadGameState(&state, &lastState, &nextState)) return;
            const uint64_t key = (uint64_t(lastState) << 12) | (uint64_t(state) << 6) | nextState;
            if (key == last) return;
            last = key;
            std::fprintf(stderr, "[twinsanityevolved] game state @ boundary %llu: %s (%u) <- %s (%u), next %s (%u)\n",
                         (unsigned long long)boundary, te::harness::StateName(state), state, te::harness::StateName(lastState), lastState,
                         te::harness::StateName(nextState), nextState);
        });
    }
    // 4d. a loaded state replaces memory, module globals, the services' state and the fiber's stack + registers
    if (!load_state.empty()) {
        std::string error;
        if (!te::savestate_runner::Load(load_state, &error)) { std::fprintf(stderr, "[twinsanityevolved] --load-state: %s\n", error.c_str()); return 6; }
    }
    // 5. run: the fiber yields at checkpoints (a save is due), at Exit, or when entry returns/traps.
    // The game's thread (this one: the fiber runs on it) at the interactive quality of service: a paced frame has
    // ~8 ms of work in a 16.7 ms field and a shared, loaded host preempts a default-QoS thread for longer than that
    // (ticks of 17-18 ms seen under load 5+ with nothing of the frame's own in them); interactive keeps it on the
    // performance cores and first in line. TE_NO_QOS=1 leaves the default (comparison runs).
    // One arm per platform in the engine (platform/host PinCurrentThreadInteractive: macOS QOS_CLASS_USER_INTERACTIVE,
    // Windows SetThreadPriority, Linux the name): the port names the thread and asks for the class.
    if (!std::getenv("TE_NO_QOS")) ps2r::platform::host::PinCurrentThreadInteractive("te-game", ps2r::platform::host::ThreadRole::Game);
    // The EE's FPU rounds every result toward zero (no IEEE rounding modes); the game's f32 arithmetic compiled to wasm
    // runs under the host's round-to-nearest unless the game thread's FPU is set to round toward zero -- the DEFAULT
    // since the clock-rate fix (TimeClockAdvanceBy's 150 vs retail's 149 clock units a frame -- the user plays at retail speed;
    // the pad logs are recorded under it). TE_EE_ROUNDING=0 keeps the host's round-to-nearest (A/B, the pre-flip logs).
    const char* ee = std::getenv("TE_EE_ROUNDING");
    if (!(ee && *ee == '0')) {
        if (std::fesetround(FE_TOWARDZERO) != 0) std::fprintf(stderr, "[twinsanityevolved] fesetround(FE_TOWARDZERO) refused\n");
        else std::fprintf(stderr, "[twinsanityevolved] fpu: rounding toward zero on the game thread (the EE's mode; TE_EE_ROUNDING=0 opts out)\n");
    } else {
        std::fprintf(stderr, "[twinsanityevolved] fpu: the host's round-to-nearest on the game thread (TE_EE_ROUNDING=0)\n");
    }
    for (;;) {
        const te::game::Reason reason = te::game::Resume();
        const te::game::Fiber& f = te::game::Get();
        if (reason == te::game::Reason::Checkpoint) {
            std::string error;
            if (!te::savestate_runner::Save(g_save_now, f.boundary, &error))
                std::fprintf(stderr, "[twinsanityevolved] --save-state %s: %s\n", g_save_now.c_str(), error.c_str());
            continue;
        }
        if (reason == te::game::Reason::LoadState) {   // the speedrun mod's practice ring -- restored in place, same process
            std::string error;
            if (!te::savestate_runner::Load(g_load_now, &error))
                std::fprintf(stderr, "[twinsanityevolved] practice state %s not loaded: %s (the game continues)\n", g_load_now.c_str(), error.c_str());
            else
                std::fprintf(stderr, "[twinsanityevolved] practice state %s loaded in place\n", g_load_now.c_str());
            continue;
        }
        if (reason == te::game::Reason::Exit) {
            if (g_watchdog) std::fprintf(stderr, "[twinsanityevolved] %s: exit 0\n", stop_at ? "--stop-at" : "--run-seconds elapsed");
            g_exit_status = g_watchdog ? 0 : f.code;
        } else if (reason == te::game::Reason::Returned) {
            std::fprintf(stderr, "[twinsanityevolved] entry returned (unexpected)\n");
            g_exit_status = 1;
        } else {
            std::fprintf(stderr, "[twinsanityevolved] wasm trap: %s\n", te_trap_name(f.code));
            g_exit_status = 70;
        }
        break;
    }
    te::padlog::Finish();
    {
        const te::Clock::Census c = te::Services::Get().clock().GetCensus();
        std::fprintf(stderr, "[twinsanityevolved] clock: %llu boundaries, %llu fields, %llu parks, %llu late, %llu timeouts (%s %u Hz); catch-up %llu boundaries, %llu fields dropped\n",
                     (unsigned long long)c.boundaries, (unsigned long long)c.fields, (unsigned long long)c.parks,
                     (unsigned long long)c.late, (unsigned long long)c.timeouts, c.play ? "play" : "deterministic", c.field_hz,
                     (unsigned long long)c.catchup_boundaries, (unsigned long long)c.dropped_fields);
        std::fprintf(stderr, "[twinsanityevolved] ticks: %llu measured, %llu over the field (%.1f ms), max %.1f ms, mean %.2f ms, cpu mean %.2f ms\n",
                     (unsigned long long)c.ticks_measured, (unsigned long long)c.ticks_over_field,
                     1000.0 / double(c.field_hz ? c.field_hz : 60u), c.tick_max_ms,
                     c.ticks_measured ? c.tick_sum_ms / double(c.ticks_measured) : 0.0,
                     c.ticks_measured ? c.tick_cpu_sum_ms / double(c.ticks_measured) : 0.0);
        te::audio::Report();   // the audio/stream/movie census (dropped=0/underruns=0)
        // the presenter's row: present=rate:shown_fps:interp_frames:snaps:latency_ms:late_presents (the frame-rate judge reads it)
        char present_row[2048] = {};
        te_graphics_present_census(present_row, sizeof present_row);
        std::fprintf(stderr, "[twinsanityevolved] %s\n", present_row);
        if (te::harness::Armed()) {
            te::harness::Set("present", std::string(present_row));
            te::harness::Set("clock", "boundaries=" + std::to_string(c.boundaries) + ",fields=" + std::to_string(c.fields) + ",parks=" +
                                          std::to_string(c.parks) + ",late=" + std::to_string(c.late) + ",timeouts=" + std::to_string(c.timeouts) +
                                          "," + (c.play ? "play" : "deterministic") + "," + std::to_string(c.field_hz) + "hz");
            const te::audio::RingHealth h = te::audio::Health();
            te::harness::Set("audio_ring", "pushed=" + std::to_string(h.pushed) + ",pulled=" + std::to_string(h.pulled) + ",dropped=" +
                                               std::to_string(h.dropped) + ",underruns=" + std::to_string(h.underruns) + ",overruns=" +
                                               std::to_string(h.overruns) + ",padded=" + std::to_string(h.padded) + ",device=" + (h.device ? "1" : "0"));
            te::harness::Set("exit_status", std::to_string(g_exit_status));
            te::harness::Set("watchdog", g_watchdog ? "1" : "0");
        }
        // the language the game ENDED in -- its current Code text table's line 9 (the main menu's "new game",
        // game/include/game/language.h g_Texts, PAL D_00309B10) beside the selection; the frozen judge reads this line.
        {
            constexpr uint32_t kTexts = 0x00309B10;   // g_Texts[CodeTexts]
            const uint8_t* mem = g_env.memory.data;
            const uint32_t lines = te::rd32(mem + kTexts);
            const char* text9 = nullptr;
            if (lines && te::GuestPointer(lines + 9 * 4, 4)) text9 = te::GuestString(te::rd32(mem + lines + 9 * 4), 128);
            std::fprintf(stderr, "[twinsanityevolved] %s; game text 9 = '%s'\n", te::language::Describe().c_str(), text9 ? text9 : "(no texts loaded)");
            if (te::harness::Armed()) te::harness::Set("language", te::language::Describe() + (text9 ? std::string("; text9=") + text9 : ""));
        }
        const te::AdaptReport ad = te::AdaptedCalls();
        std::fprintf(stderr, "[twinsanityevolved] call_indirect adapted: %llu calls at %u distinct sites (adapted at the call site; 0 traps)\n",
                     (unsigned long long)ad.calls, ad.sites);
        // The game's heap (game/src/game/memory.cpp HeapManager g_HeapManager, PAL D_0031B338): the big-block part and the
        // small allocations' pages. A frame leak shows here as pages used == pages (the game then gets NULL from its
        // allocator and corrupts low memory): the frozen judge tools/judges/padlog_judge.py reads this line.
        {
            constexpr uint32_t kHeapManager = 0x0031B338;
            const uint8_t* mem = g_env.memory.data;
            const uint32_t ready = te::rd32(mem + kHeapManager + 0x30);
            if (ready == 1) {
                const uint32_t pool_size = te::rd32(mem + kHeapManager + 0x20), free_bytes = te::rd32(mem + kHeapManager + 0x14);
                const uint32_t used_bytes = te::rd32(mem + kHeapManager + 0x18), peak = te::rd32(mem + kHeapManager + 0x2C);
                const uint32_t pages = te::rd32(mem + kHeapManager + 0x40), pages_free = te::rd32(mem + kHeapManager + 0x64);
                const uint32_t pool_start = te::rd32(mem + kHeapManager + 0x1C);   // HeapManager::poolStart (fixed per build)
                std::fprintf(stderr, "[twinsanityevolved] heap: pool %u bytes at 0x%08X, used %u, peak %u, free %u; small pages used %u of %u (free %u)\n",
                             pool_size, pool_start, used_bytes, peak, free_bytes, pages - pages_free, pages, pages_free);
                te::harness::Set("heap", "pool=" + std::to_string(pool_size) + ",used=" + std::to_string(used_bytes) + ",peak=" + std::to_string(peak) +
                                             ",free=" + std::to_string(free_bytes) + ",pages_used=" + std::to_string(pages - pages_free) + ",pages=" + std::to_string(pages));
            } else {
                std::fprintf(stderr, "[twinsanityevolved] heap: not started\n");
            }
        }
    }
    te::harness::Finalize();   // the leg's tails + run.meta (after the census keys)
    wasm2c_game_free(&instance);
    return g_exit_status;
}
