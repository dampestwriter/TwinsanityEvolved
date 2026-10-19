// te_input.h -- the pads' host sampler over the engine's SDL InputSystem (host/te_input.cpp). The window owner
// (graphics) coordinates the SDL event pump through SetPumpOwned/HandleSdlEvent.
#pragma once
#include <cstdint>
#include <string>

#include "te_host.h"

union SDL_Event;

namespace te {
class Pads;
namespace input {

// Installs the SDL sampler on `pads` (pad 0). False when SDL input is off or unavailable: the neutral pad stays.
bool Install(Pads& pads, const Settings& settings);
// True (default): this module polls SDL events at the boundary. The window owner sets false and forwards events.
void SetPumpOwned(bool owned);
bool PumpOwned();
#if TE_HAVE_SDL_INPUT
void HandleSdlEvent(const SDL_Event& event);
#endif
// "kbm=on|off,gamepad=connected|absent,active=..." (the engine's census string), "none" without SDL input.
std::string Sources();
// The keyboard harness ("menus navigable with the keyboard" check without an OS event injector): the SDL
// key NAME ("Y", "Down", "Space"...) reads as HELD through the engine's KeyboardMouseBackend::InjectKey -- its bindings
// table and InputMapper, the path a real key takes after SDL_GetKeyboardState -- at every boundary in [from, to].
// False when the name is not an SDL key. Takes effect once Install has run (the entries are kept until then).
bool InjectKey(const std::string& key_name, uint64_t from, uint64_t to, std::string* error);
// The speedrun mod's keys: whether the SDL key of that NAME ("F9") is held right now, by SDL's keyboard state (the pump
// keeps it current); false without SDL input or for an unknown name. A harness --inject-key of the name reads as held too.
bool KeyHeld(const char* key_name);
// The window-close harness: an SDL_QUIT in the event queue (what the window's close button posts); the window owner's
// pump (ps2e_draw_pump) takes it at the next boundary and the runner exits 0 through te::RequestExit.
bool RequestWindowClose();
void Shutdown();

}  // namespace input
}  // namespace te
