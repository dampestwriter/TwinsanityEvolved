// te_host.h -- what every host-side seam module (platform/nativeee/host/<header>.cpp) may use: guest memory access by
// u32 address (wasm address == guest address == MainMemory offset), the run-control hooks the runner provides, and the
// run settings the runner parsed (the disc image path is the manifest: every file the game reads is read out of it at
// RUN time).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace te {

// The 32 MB linear memory the module and the engine share. Null until the runner bound it.
uint8_t* GuestMemory();
std::size_t GuestMemorySize();

// A host pointer to `size` guest bytes at `address`, or nullptr when the range is outside the 32 MB.
inline uint8_t* GuestPointer(uint32_t address, std::size_t size) {
    uint8_t* mem = GuestMemory();
    if (!mem || uint64_t(address) + size > GuestMemorySize()) return nullptr;
    return mem + address;
}
// A NUL-terminated guest string of at most `max` bytes, or nullptr. Not copied: valid until the game writes there.
const char* GuestString(uint32_t address, std::size_t max);

// Platform::System::Exit arrives here: the runner leaves the module (longjmp out of wasm2c) and ends with `status`.
[[noreturn]] void RequestExit(int status);

}  // namespace te

// A `void()` callback the game handed a host body as a function pointer (a wasm TABLE INDEX): called back into the
// module. Returns 1 when it was called, 0 when the index names no `() -> nil` function (logged) or there is no module.
// The runner defines it (runner/te_env.cpp); the tests' harness defines a counting one; a host executable without
// either (the graphics self-test) gets the weak default in movie_player.cpp, which returns 0.
extern "C" int te_call_void_callback(uint32_t index);

namespace te {

// What the runner (or a test) decided before the module is entered. Read by the services at construction
// (te::Services, host/te_services.h); never read from the environment or a file by the services themselves.
struct Settings {
    std::string iso_path;        // the disc image: the manifest. Empty = no disc (tests; Disc::Initialise fails)
    std::string mc_dir;          // the memory cards' host directory (<mc_dir>/mc0, mc1). Empty = saves see no card
    // te_language.h: the game's language. `language_name` is the name asked for (ps2r.ini game.language /
    // --language <name>; "" = english), resolved against the disc's text banks by CONTENT; `language_bank` (>= 0) names a
    // slot of the game's language table directly (--language-bank N: a diagnostic, the judge's raw arm); `language` (>= 0)
    // is a raw Platform::System::ConsoleLanguage code (--language 0..7, the raw-code form; what Language() answers when the
    // disc's banks cannot be enumerated -- the tests' fake disc). -1 / "" = not given.
    std::string language_name;
    int32_t language_bank = -1;
    int32_t language = -1;
    std::string settings_path;   // the settings file (ps2r.ini) the Options menu persists to; "" = nothing is written
    bool deterministic = false;  // TickSource Deterministic: no pacer thread, one field per boundary, fixed RTC
    uint32_t field_hz = 60;      // the pacer's field rate (charter: game_hz 60; 50 reproduces a PAL console)
    bool kbm = true;             // keyboard+mouse feed pad 0 (engine InputSystem) when SDL input is available
    bool sdl_input = true;       // false: no SDL at all, pad 0 is the neutral connected pad (headless runs, tests)
    bool pad_connected = true;   // the neutral pad reports connected (the game then completes its DualShock 2 setup)
    bool skip_movies = false;    // FEATURES "skip movies": every movie ends at its first Step (the stub's behaviour)
    bool stream_instant = false; // the stream façade's INSTANT model (reads complete inside the call) instead of the
                                 // engine's drive-time model (ReadTiming: seek + transfer, poll/boundary/wait visibility)
    bool present_ct16 = false;   // the retail present -- the frame shown as the GS's dithered 16-bit copy (CT24 -> CT16)
    bool verbose = false;
};
const Settings& GetSettings();
void SetSettings(const Settings& settings);

}  // namespace te
