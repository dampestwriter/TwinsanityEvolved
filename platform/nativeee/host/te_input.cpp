// platform/nativeee (host side) -- the pads' host sampler: the engine's SDL InputSystem (a game controller when one is
// connected, keyboard+mouse ALWAYS live as a pad, hot-plug both ways, the engine's default_bindings.ini) feeding pad 0,
// polled once per frame boundary on the game thread (SDL's "main thread" is the runner's, which runs the module).
//
// Event pump ownership: with no window (headless runs) this module pumps SDL's events itself at the boundary and
// forwards them to the InputSystem; the window owner (graphics) may take the pump -- te::input::SetPumpOwned(false)
// and te::input::HandleSdlEvent(e) for every event it polls -- so keyboard events reach the pad through one queue.
// Without SDL input (te::Settings::sdl_input false, or SDL refusing to start) the neutral connected pad stays.
#include <cstdio>
#include <vector>

#include "te_input.h"
#include "te_services.h"

#if TE_HAVE_SDL_INPUT
#include <SDL.h>

#include "engine/platform/sdl/input/input_system.h"
#include "engine/platform/sdl/input/keyboard_mouse_backend.h"
#endif

namespace te::input {

namespace {
#if TE_HAVE_SDL_INPUT
ps2r::platform::sdl::InputSystem* g_input = nullptr;
bool g_events_inited = false;
#endif
bool g_pump_owned = true;
bool g_installed = false;
struct Injection { int scancode; uint64_t from, to; };
std::vector<Injection> g_injections;
}  // namespace

bool Install(Pads& pads, const Settings& settings) {
    if (g_installed) return true;
    if (!settings.sdl_input) return false;
#if TE_HAVE_SDL_INPUT
    if (!SDL_WasInit(SDL_INIT_EVENTS)) {
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            std::fprintf(stderr, "[twinsanityevolved] input: SDL events unavailable (%s); neutral pad\n", SDL_GetError());
            return false;
        }
        g_events_inited = true;
    }
    g_input = new ps2r::platform::sdl::InputSystem;
    ps2r::platform::sdl::InputSettings in;
    in.kbm_enabled = settings.kbm;
    in.gamepad_enabled = true;
    g_input->Init(in);
    const uint32_t hz = settings.field_hz ? settings.field_hz : 60u;
    const float dt = 1.0f / static_cast<float>(hz);
    pads.SetSampler([dt](unsigned port, unsigned slot) -> Pads::Sample {
        Pads::Sample s;
        if (port != 0 || slot != 0) return s;   // one pad: the engine's InputSystem is pad 0
        if (g_pump_owned) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) g_input->HandleEvent(e);
        }
        if (!g_injections.empty() && g_input->Kbm()) {
            const uint64_t boundary = te::Services::Get().clock().BoundaryCount();
            for (const Injection& k : g_injections)
                if (boundary >= k.from && boundary <= k.to) g_input->Kbm()->InjectKey(k.scancode);
        }
        const ps2r::platform::sdl::PadSample p = g_input->Frame(dt);
        s.connected = g_input->Connected() || te::GetSettings().pad_connected;
        s.buttons = p.buttons;
        s.lx = p.lx; s.ly = p.ly; s.rx = p.rx; s.ry = p.ry;
        return s;
    });
    g_installed = true;
    if (settings.verbose) std::fprintf(stderr, "[twinsanityevolved] input: SDL InputSystem on pad 0 (%s)\n", g_input->Sources().c_str());
    return true;
#else
    (void)pads;
    return false;
#endif
}

bool InjectKey(const std::string& key_name, uint64_t from, uint64_t to, std::string* error) {
#if TE_HAVE_SDL_INPUT
    const SDL_Scancode sc = SDL_GetScancodeFromName(key_name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) { if (error) *error = "not an SDL key name: " + key_name; return false; }
    g_injections.push_back({static_cast<int>(sc), from, to});
    return true;
#else
    (void)from; (void)to;
    if (error) *error = "no SDL input in this build: " + key_name;
    return false;
#endif
}

bool KeyHeld(const char* key_name) {
#if TE_HAVE_SDL_INPUT
    const SDL_Scancode sc = SDL_GetScancodeFromName(key_name);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    if (!g_injections.empty()) {
        const uint64_t boundary = te::Services::Get().clock().BoundaryCount();
        for (const Injection& k : g_injections)
            if (k.scancode == int(sc) && boundary >= k.from && boundary <= k.to) return true;
    }
    if (!g_installed || !SDL_WasInit(SDL_INIT_EVENTS)) return false;
    int count = 0;
    const Uint8* state = SDL_GetKeyboardState(&count);
    return state && int(sc) < count && state[sc] != 0;
#else
    (void)key_name;
    return false;
#endif
}

bool RequestWindowClose() {
#if TE_HAVE_SDL_INPUT
    SDL_Event e{};
    e.type = SDL_QUIT;
    return SDL_PushEvent(&e) == 1;
#else
    return false;
#endif
}

void SetPumpOwned(bool owned) { g_pump_owned = owned; }
bool PumpOwned() { return g_pump_owned; }

#if TE_HAVE_SDL_INPUT
void HandleSdlEvent(const SDL_Event& event) { if (g_input) g_input->HandleEvent(event); }
#endif

std::string Sources() {
#if TE_HAVE_SDL_INPUT
    return g_input ? g_input->Sources() : std::string("none");
#else
    return "none";
#endif
}

void Shutdown() {
#if TE_HAVE_SDL_INPUT
    if (g_input) { g_input->Shutdown(); delete g_input; g_input = nullptr; }
    if (g_events_inited) { SDL_QuitSubSystem(SDL_INIT_EVENTS); g_events_inited = false; }
#endif
    g_installed = false;
}

}  // namespace te::input
