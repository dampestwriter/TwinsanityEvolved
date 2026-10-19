// te_texts.h -- the platform's own texts over the frame, drawn THROUGH THE GAME'S TEXT SYSTEM. Once a frame
// the game calls Platform::Graphics::QueuePlatformTexts (the wasm side, graphics_text.cpp), which asks the host for up
// to a few te_platform_text records (ps2e_gfx2_platform_texts) and queues each with the game's QueueText in the game's
// font -- the same glyph quads the menus and the HUD are drawn with, never the ImGui overlay. Sources register here: the
// fps counter (display.fps_counter: render fps + game Hz top-right) and the mods' lines (the speedrun timer).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace te::texts {

// One line: fractions of the screen (x, y), the glyph scale (1 = the menu's size), RGBA bytes, game/font.h
// TextAlignment::Mask bits, the text ('~' starts a new line; at most 63 characters cross).
struct Line {
    float x = 0.5f, y = 0.5f, scale = 0.5f;
    uint32_t colour = 0xFFFFFFFFu;   // RGBA bytes: 0x80 alpha is the GS's opaque, the game's menu white is 0x80FFFFFF
    uint32_t alignment = 0x11;       // TopLeft
    std::string text;
};
enum Alignment : uint32_t { Top = 0x1, Centre = 0x2, Bottom = 0x4, Left = 0x10, Middle = 0x20, Right = 0x40 };

using Source = std::function<void(std::vector<Line>& out)>;
// A source is asked every frame, in registration order; returns its id (for Unregister).
int Register(const std::string& name, Source source);
void Unregister(int id);
// Every registered source's lines for this frame (the import ps2e_gfx2_platform_texts calls it).
void Collect(std::vector<Line>& out);
// How many lines the last frame carried (diagnostics / the judge's log line).
uint32_t LastCount();

// The fps counter's current figures (render fps, game Hz over the last window; 0 before the first window closes).
struct Rates { double fps = 0, hz = 0; };
Rates CurrentRates();
// Registers the fps counter source (idempotent; display.fps_counter decides whether it emits).
void InstallFpsCounter();

}  // namespace te::texts
