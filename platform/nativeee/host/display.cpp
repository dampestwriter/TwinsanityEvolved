// platform/nativeee (host side) -- te_display.h: the display settings. See the header.
//
// The engine's Display page rows (aspect, internal scale, texture filter, vsync, window mode) over ps2e_draw's
// presentation; the title's MoveDisplay as the picture's offset; every change pushed to the device at once and written
// to ps2r.ini through the engine's SettingsFile (the keys are the engine's VideoConfig: video.aspect, video.scale,
// video.texture_filter, video.vsync, video.fullscreen, video.custom_offset_x/y). The rows' labels come from the engine's
// MenuStrings (the title's menu extension's strings, per language bank: English, French, German, Spanish, Italian;
// another language -- the patched disc's Portuguese -- reads the English labels).
#include "te_display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "engine/core/config/settings_file.h"
#include "engine/platform/ps2e/present_pacer.h"
#include "engine/platform/ps2e/ps2e_draw.h"
#include "games/slus_20909_crash_twinsanity/hle/menu/menu_strings.h"
#include "graphics_internal.h"
#include "ps2e_seam.h"
#include "te_host.h"
#include "te_language.h"
#include "te_services.h"

namespace te::display {

namespace {

Settings g_settings;
uint64_t g_page_polled_at = 0;   // the boundary at which the game last read a row's value (the page is open)
using Pacer = ps2r::ps2e::PresentPacer;
bool Uncapped() { return g_settings.frame_rate == int(Pacer::Rate::Uncapped); }

const char* const kAspectKeys[] = {"auto", "4:3", "16:9", "16:10", "21:9"};
const char* const kFullscreenKeys[] = {"windowed", "fullscreen", "borderless"};
const char* const kFilterKeys[] = {"title", "point", "bilinear", "trilinear"};

// The language's name for MenuStrings ("English", "French", ...): the selected language (its TEXT's language), capitalised;
// an unknown one (Portuguese) falls back to English inside MenuStrings.
std::string MenuLanguage() {
    const language::Table& t = language::Resolve();
    std::string name = t.selected >= 0 && size_t(t.selected) < t.languages.size() ? t.languages[size_t(t.selected)].name : "english";
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') name[0] = char(name[0] - 'a' + 'A');
    return name;
}

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string MS(title::MenuStrings::Id id) { return Lower(title::MenuStrings::Text(id, MenuLanguage())); }

void Persist(const char* key, const std::string& value) {
    const std::string& path = GetSettings().settings_path;
    if (path.empty()) return;
    std::string err;
    if (!ps2r::SettingsFile::Set(path, key, value, &err))
        std::fprintf(stderr, "[twinsanityevolved] display: %s = %s not written to %s: %s\n", key, value.c_str(), path.c_str(), err.c_str());
}

}  // namespace

void Configure(const Settings& settings) {
    g_settings = settings;
    g_settings.aspect = std::clamp(g_settings.aspect, 0, 4);
    g_settings.fit = std::clamp(g_settings.fit, 0, 4);
    g_settings.scale = std::clamp(g_settings.scale, 1, 4);
    g_settings.texture_filter = std::clamp(g_settings.texture_filter, 0, 3);
    g_settings.fullscreen = std::clamp(g_settings.fullscreen, 0, 2);
    g_settings.offset_x = std::clamp(g_settings.offset_x, -256, 256);
    g_settings.offset_y = std::clamp(g_settings.offset_y, -256, 256);
    g_settings.frame_rate = std::clamp(g_settings.frame_rate, 0, 5);
    Apply();
}

const Settings& Current() { return g_settings; }

void Apply() {
    ps2e_draw_presentation p{};
    const bool headless = gfx::Headless();
    // The shape: `auto` is the TV's 4:3 in a window and the frame's own pixels headless (the frozen judges' captures are
    // the frame as drawn); the explicit shapes apply either way (the aspect judge runs headless with 16:9).
    switch (g_settings.aspect) {
    case 1: p.aspect = PS2E_DRAW_ASPECT_4_3; break;
    case 2: p.aspect = PS2E_DRAW_ASPECT_16_9; break;
    case 3: p.aspect = PS2E_DRAW_ASPECT_16_10; break;
    case 4: p.aspect = PS2E_DRAW_ASPECT_21_9; break;
    default: p.aspect = headless ? PS2E_DRAW_ASPECT_PIXEL : PS2E_DRAW_ASPECT_4_3; break;
    }
    p.fit = g_settings.fit == 2 ? PS2E_DRAW_FIT_FILL : g_settings.fit == 3 ? PS2E_DRAW_FIT_INTEGER : PS2E_DRAW_FIT_LETTERBOX;
    if (g_settings.fit == 0) p.aspect = PS2E_DRAW_ASPECT_WINDOW;   // stretch
    p.offset_x = g_settings.offset_x;
    p.offset_y = g_settings.offset_y;
    p.nearest = 0;
    p.vsync = EffectiveVSync() ? 1 : 0;   // uncapped forces the swap interval off for the session
    p.window_mode = headless ? PS2E_DRAW_WINDOWED : g_settings.fullscreen;
    p.scale = g_settings.scale;
    p.window_scale = 0;   // the window keeps its size (2x the frame at creation; the player resizes it)
    p.texture_filter = g_settings.texture_filter == 0 ? 0 : g_settings.texture_filter == 1 ? 1 : 2;
    gfx::SetPresentation(p);
    gfx::SetFrameRate(g_settings.frame_rate);
}

void MoveDisplay(float x, float y) {
    const float cx = std::clamp(x, -1.f, 1.f), cy = std::clamp(y, -1.f, 1.f);
    const int ox = int(std::lround(cx * 64.f)), oy = int(std::lround(cy * 32.f));   // the PS2's 256 VCK = 64 frame pixels, 32 lines
    if (ox == g_settings.offset_x && oy == g_settings.offset_y) return;
    g_settings.offset_x = ox;
    g_settings.offset_y = oy;
    Apply();
    Persist("video.custom_offset_x", std::to_string(ox));
    Persist("video.custom_offset_y", std::to_string(oy));
}

std::string Describe() {
    char buf[256];
    std::snprintf(buf, sizeof buf, "display: aspect %s, fit %d, scale %dx, texture filter %s, vsync %s%s, frame rate %s, %s, offset (%d, %d), fps counter %s%s",
                  kAspectKeys[g_settings.aspect], g_settings.fit, g_settings.scale, kFilterKeys[g_settings.texture_filter],
                  EffectiveVSync() ? "on" : "off", Uncapped() && g_settings.vsync ? " (forced off: uncapped)" : "", FrameRateName(),
                  kFullscreenKeys[g_settings.fullscreen], g_settings.offset_x, g_settings.offset_y,
                  g_settings.fps_counter ? "on" : "off", gfx::Headless() ? " (headless: auto = the frame's pixels, windowed)" : "");
    return buf;
}

uint32_t Choices(uint32_t row) {
    switch (row) {
    case RowAspect: return 5;
    case RowScale: return 4;
    case RowTextureFilter: return 3;
    case RowVSync: return 2;
    case RowFrameRate: return 6;
    case RowDisplayMode: return 3;
    case RowFpsCounter: return 2;
    default: return 0;
    }
}

bool FpsCounter() { return g_settings.fps_counter; }
void SetFpsCounter(bool on) { g_settings.fps_counter = on; }
int FrameRate() { return g_settings.frame_rate; }
const char* FrameRateName() { return Pacer::Name(static_cast<Pacer::Rate>(g_settings.frame_rate)); }
bool EffectiveVSync() { return g_settings.vsync && !Uncapped(); }
std::string FrameRateHelp() { return MS(title::MenuStrings::Id::VSyncCapsRate); }
bool GraphicsPageOpen() {
    const uint64_t now = Services::Get().clock().BoundaryCount();
    return g_page_polled_at != 0 && now >= g_page_polled_at && now - g_page_polled_at <= 1;
}

std::string RowLabel(uint32_t row) {
    using Id = title::MenuStrings::Id;
    switch (row) {
    case RowAspect: return MS(Id::Aspect);
    case RowScale: return MS(Id::Scale);
    case RowTextureFilter: return MS(Id::TextureFilter);
    case RowVSync: return MS(Id::VSync);
    case RowFrameRate: return MS(Id::FrameRate);
    case RowDisplayMode: return MS(Id::DisplayMode);
    case RowFpsCounter: return MS(Id::PerfOverlay);   // "fps overlay": the engine's name for the same row
    default: return "";
    }
}

std::string ChoiceLabel(uint32_t row, uint32_t choice) {
    using Id = title::MenuStrings::Id;
    switch (row) {
    case RowAspect: {
        static const Id ids[] = {Id::AspectAuto, Id::AspectNormal, Id::AspectWide, Id::AspectWideTen, Id::AspectUltraWide};
        return choice < 5 ? MS(ids[choice]) : "";
    }
    case RowScale: {
        static const Id ids[] = {Id::Scale1x, Id::Scale2x, Id::Scale3x, Id::Scale4x};
        return choice < 4 ? MS(ids[choice]) : "";
    }
    case RowTextureFilter: {
        static const Id ids[] = {Id::FilterTitle, Id::FilterPoint, Id::FilterBilinear};
        return choice < 3 ? MS(ids[choice]) : "";
    }
    case RowVSync: return choice == 0 ? (Uncapped() ? MS(Id::OffUncapped) : MS(Id::Aniso1) /* "off" */) : "on";
    case RowFrameRate: {   // digits, as the brief spells them; the last one the engine's word
        if (choice == 5) return MS(Id::Uncapped);
        return choice < 5 ? Pacer::Name(static_cast<Pacer::Rate>(choice)) : "";
    }
    case RowFpsCounter: return choice == 0 ? MS(Id::Aniso1) /* "off" */ : "on";
    case RowDisplayMode: {
        static const Id ids[] = {Id::Windowed, Id::Fullscreen, Id::Borderless};
        return choice < 3 ? MS(ids[choice]) : "";
    }
    default: return "";
    }
}

uint32_t Value(uint32_t row) {
    g_page_polled_at = Services::Get().clock().BoundaryCount();   // the page's Frame reads every row each frame
    switch (row) {
    case RowAspect: return uint32_t(g_settings.aspect);
    case RowScale: return uint32_t(g_settings.scale - 1);
    case RowTextureFilter: return uint32_t(std::min(g_settings.texture_filter, 2));
    case RowVSync: return EffectiveVSync() ? 1u : 0u;   // uncapped: off, and the row's label says why
    case RowFrameRate: return uint32_t(g_settings.frame_rate);
    case RowDisplayMode: return uint32_t(g_settings.fullscreen);
    case RowFpsCounter: return g_settings.fps_counter ? 1u : 0u;
    default: return 0;
    }
}

void Set(uint32_t row, uint32_t choice) {
    if (choice >= Choices(row)) return;
    switch (row) {
    case RowAspect: g_settings.aspect = int(choice); Persist("video.aspect", kAspectKeys[choice]); break;
    case RowScale: g_settings.scale = int(choice) + 1; Persist("video.scale", std::to_string(g_settings.scale)); break;
    case RowTextureFilter: g_settings.texture_filter = int(choice); Persist("video.texture_filter", kFilterKeys[choice]); break;
    case RowVSync:
        if (Uncapped()) {   // not editable while uncapped: the swap interval is the session's, forced off
            std::fprintf(stderr, "[twinsanityevolved] display: vsync is off while the frame rate is uncapped (not changed)\n");
            return;
        }
        g_settings.vsync = choice != 0;
        Persist("video.vsync", g_settings.vsync ? "on" : "off");
        break;
    case RowFrameRate:
        g_settings.frame_rate = int(choice);
        Persist("video.frame_rate", Pacer::Name(static_cast<Pacer::Rate>(choice)));
        break;
    case RowDisplayMode: g_settings.fullscreen = int(choice); Persist("video.fullscreen", kFullscreenKeys[choice]); break;
    case RowFpsCounter:   // nothing of the device's -- the next frame's QueuePlatformTexts reads it
        g_settings.fps_counter = choice != 0;
        Persist("display.fps_counter", g_settings.fps_counter ? "on" : "off");
        std::fprintf(stderr, "[twinsanityevolved] %s\n", Describe().c_str());
        return;
    default: return;
    }
    Apply();
    std::fprintf(stderr, "[twinsanityevolved] %s\n", Describe().c_str());
}

}  // namespace te::display

extern "C" {

// Platform::Graphics::DisplaySetting*: the graphic options page's platform rows
ps2e_s32 ps2e_graphics_display_setting_count(void) { return static_cast<ps2e_s32>(te::display::RowCount); }

ps2e_s32 ps2e_graphics_display_setting_choices(ps2e_s32 setting) {
    return setting >= 0 ? static_cast<ps2e_s32>(te::display::Choices(static_cast<uint32_t>(setting))) : 0;
}

namespace {
void CopyLabel(const std::string& label, ps2e_u32 out, ps2e_s32 size) {
    if (size <= 0) return;
    uint8_t* p = te::GuestPointer(out, static_cast<size_t>(size));
    if (!p) return;
    const size_t n = std::min(label.size(), static_cast<size_t>(size) - 1);
    std::copy_n(label.data(), n, reinterpret_cast<char*>(p));
    p[n] = 0;
}
}  // namespace

void ps2e_graphics_display_setting_label(ps2e_s32 setting, ps2e_u32 out, ps2e_s32 size) {
    CopyLabel(setting >= 0 ? te::display::RowLabel(static_cast<uint32_t>(setting)) : "", out, size);
}

void ps2e_graphics_display_setting_choice_label(ps2e_s32 setting, ps2e_s32 choice, ps2e_u32 out, ps2e_s32 size) {
    CopyLabel(setting >= 0 && choice >= 0 ? te::display::ChoiceLabel(static_cast<uint32_t>(setting), static_cast<uint32_t>(choice)) : "", out, size);
}

ps2e_s32 ps2e_graphics_display_setting_value(ps2e_s32 setting) {
    return setting >= 0 ? static_cast<ps2e_s32>(te::display::Value(static_cast<uint32_t>(setting))) : 0;
}

void ps2e_graphics_set_display_setting(ps2e_s32 setting, ps2e_s32 choice) {
    if (setting >= 0 && choice >= 0) te::display::Set(static_cast<uint32_t>(setting), static_cast<uint32_t>(choice));
}

}  // extern "C"
