#include "engine/platform/sdl/input/input_system.h"
#include "engine/platform/sdl/input/gamepad_backend.h"
#include "engine/platform/sdl/input/keyboard_mouse_backend.h"
#include "games/slus_20909_crash_twinsanity/hle/input/input.h"

#include <SDL.h>
#include <cstdio>

namespace {
using namespace ps2r::platform::sdl;
int checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)
void Key(InputSystem& input, Uint32 type, SDL_Scancode code, bool repeat = false) {
    SDL_Event event{};
    event.type = type;
    event.key.keysym.scancode = code;
    event.key.repeat = repeat;
    input.HandleEvent(event);
}
void Mouse(InputSystem& input, Uint32 type, Uint8 button) {
    SDL_Event event{};
    event.type = type;
    event.button.button = button;
    input.HandleEvent(event);
}
void Focus(InputSystem& input, Uint8 state) {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.event = state;
    input.HandleEvent(event);
}
void Button(InputSystem& input, Sint32 id, Uint32 type, Uint8 button) {
    SDL_Event event{};
    event.type = type;
    event.cbutton.which = id;
    event.cbutton.button = button;
    input.HandleEvent(event);
}
void Trigger(InputSystem& input, Sint32 id, Sint16 value) {
    SDL_Event event{};
    event.type = SDL_CONTROLLERAXISMOTION;
    event.caxis.which = id;
    event.caxis.axis = SDL_CONTROLLER_AXIS_TRIGGERLEFT;
    event.caxis.value = value;
    input.HandleEvent(event);
}
void KeyboardAndMouse() {
    InputSystem input;
    InputSettings settings;
    settings.gamepad_enabled = false;
    input.Init(settings);
    CHECK(input.Connected());
    CHECK(input.Mapper().LoadConfigText(
        "[buttons]\ncross = Space\nstart = Y\ncircle = Mouse2\n[sticks]\n"
        "key_left_up = W\nkey_left_down = S\nkey_left_left = A\nkey_left_right = D\n", "edge-test"));
    CHECK(input.Frame(0).buttons == 0);
    // OLD R183 root1 reconciled with PORT_COMPATIBILITY I2: consume short
    // presses once per game sample. No arbitrary libpad-read-count latch.
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE);
    Key(input, SDL_KEYUP, SDL_SCANCODE_SPACE);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_Y);
    Key(input, SDL_KEYUP, SDL_SCANCODE_Y);
    const auto tap = input.Frame(0);
    CHECK(tap.buttons == (kBtnCross | kBtnStart));
    CHECK(input.Frame(0).buttons == 0);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE, true);
    CHECK(input.Frame(0).buttons == 0); // repeat cannot manufacture a tap
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_W);
    Key(input, SDL_KEYUP, SDL_SCANCODE_W);
    CHECK(input.Frame(0).ly == 128); // sticks deliberately do not latch
    Mouse(input, SDL_MOUSEBUTTONDOWN, SDL_BUTTON_RIGHT);
    Mouse(input, SDL_MOUSEBUTTONUP, SDL_BUTTON_RIGHT);
    CHECK(input.Frame(0).buttons == kBtnCircle);
    CHECK(input.Frame(0).buttons == 0);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE);
    Focus(input, SDL_WINDOWEVENT_FOCUS_LOST);
    Key(input, SDL_KEYUP, SDL_SCANCODE_SPACE);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_Y);
    Key(input, SDL_KEYUP, SDL_SCANCODE_Y);
    CHECK(input.Frame(0).buttons == 0);
    NormalizedInput merged;
    merged.buttons = kBtnCircle;
    input.Kbm()->Poll(merged);
    CHECK(merged.buttons == kBtnCircle); // suppression preserves earlier sources
    Focus(input, SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(input.Connected()); // focus did not manufacture a disconnect
    CHECK(input.Frame(0).buttons == 0); // stale edge does not escape on refocus
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE);
    input.SetUiCapture(true);
    CHECK(input.Frame(0).buttons == 0);
    Key(input, SDL_KEYUP, SDL_SCANCODE_SPACE);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_Y);
    Key(input, SDL_KEYUP, SDL_SCANCODE_Y);
    Mouse(input, SDL_MOUSEBUTTONDOWN, SDL_BUTTON_RIGHT);
    Mouse(input, SDL_MOUSEBUTTONUP, SDL_BUTTON_RIGHT);
    input.SetUiCapture(false); // even if no Frame ran while captured
    CHECK(input.Frame(0).buttons == 0);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE);
    input.SetKbmEnabled(false);
    input.SetKbmEnabled(true);
    CHECK(input.Frame(0).buttons == 0);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE);
    Key(input, SDL_KEYUP, SDL_SCANCODE_SPACE);
    CHECK(input.Frame(0).buttons == kBtnCross); // fresh input works again
    CHECK(input.Frame(0).buttons == 0);
    input.Kbm()->InjectKey(SDL_SCANCODE_SPACE);
    CHECK(input.Frame(0).buttons == kBtnCross);
    CHECK(input.Frame(0).buttons == 0);
    Key(input, SDL_KEYDOWN, SDL_SCANCODE_SPACE);
    Key(input, SDL_KEYUP, SDL_SCANCODE_SPACE);
    input.ResetPending();
    CHECK(input.Frame(0).buttons == 0); // reset/restore cannot carry an old tap

    title::Input service;
    CHECK(service.Open(0, 0) == 1);
    std::array<title::Input::Sample, 8> samples{};
    samples[0].connected = true;
    service.SampleBoundary(8, samples);
    CHECK(service.EnterPressure(0, 0) == 1);
    samples[0].buttons = tap.buttons;
    service.SampleBoundary(9, samples);
    const auto served = service.Current(0, 0);
    CHECK(served.sample.buttons == tap.buttons && served.sample.connected);
    CHECK(served.report[14] == 255 && served.report[2] == 0xf7 && served.report[3] == 0xbf);
    title::Input::Report repeated{};
    CHECK(service.Read(0, 0, repeated) == 32 && repeated == served.report);
    samples[0].buttons = input.Frame(0).buttons;
    service.SampleBoundary(10, samples);
    CHECK(service.Current(0, 0).report[14] == 0);
}
void Gamepads() {
    const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 21, 0);
    CHECK(device >= 0);
    if (device < 0) return;
    {
        InputSystem input;
        InputSettings settings;
        settings.kbm_enabled = false;
        input.Init(settings);
        CHECK(input.Connected());
        CHECK(input.Gamepad()->Snapshot().size() == 1);
        if (input.Gamepad()->Snapshot().empty()) return;
        const Sint32 id = input.Gamepad()->Snapshot().front().instance_id;
        CHECK(input.Frame(0).buttons == 0);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        Button(input, id, SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A);
        CHECK(input.Frame(0).buttons == kBtnCross);
        CHECK(input.Frame(0).buttons == 0);
        Trigger(input, id, 20000);
        Trigger(input, id, 0);
        CHECK(input.Frame(0).buttons == kBtnL2);
        CHECK(input.Frame(0).buttons == 0);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_START);
        input.SetUiCapture(true);
        Button(input, id, SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_START);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        Button(input, id, SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A);
        input.SetUiCapture(false);
        CHECK(input.Frame(0).buttons == 0);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        Focus(input, SDL_WINDOWEVENT_FOCUS_LOST);
        CHECK(input.Frame(0).buttons == 0);
        Focus(input, SDL_WINDOWEVENT_FOCUS_GAINED);
        CHECK(input.Frame(0).buttons == 0);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        input.Gamepad()->SetSelection(GamepadBackend::kSelectionNone);
        input.Gamepad()->SetSelection(GamepadBackend::kSelectionAuto);
        CHECK(input.Frame(0).buttons == 0);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        input.Gamepad()->SetEnabled(false);
        input.Gamepad()->SetEnabled(true);
        CHECK(input.Frame(0).buttons == 0);
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        input.ResetPending();
        CHECK(input.Frame(0).buttons == 0);
        // Device removal discards that device's unconsumed edge.
        Button(input, id, SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A);
        SDL_Event removed{};
        removed.type = SDL_JOYDEVICEREMOVED;
        removed.jdevice.which = id;
        input.HandleEvent(removed);
        CHECK(!input.Gamepad()->IsActive() && input.Frame(0).buttons == 0);
        CHECK(!input.Connected());
    }
    CHECK(SDL_JoystickDetachVirtual(device) == 0);
}
}
int main() {
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0);
    KeyboardAndMouse();
    Gamepads();
    SDL_Quit();
    std::printf("InputEdges: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
