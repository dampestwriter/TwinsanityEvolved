#include "games/slus_20909_crash_twinsanity/composition/menu_composition.h"

#include <functional>
#include <utility>
#include <vector>

#include "app/host/session_runtime.h"
#include "engine/ee/cpu/dispatch_table.h"
#include "engine/ee/recomp_abi/host_app/memory.h"

namespace ps2r::app {

MenuComposition::MenuComposition(SessionRuntime* session) : extension_(Guest()) {
    if (!session) return;
    Declare(extension_, *session);
    installed_ = extension_.Install([](uint32_t a) { return DispatchTable::Capture(a); },
                                    [](uint32_t a, title::MenuExtension::Body b) { return DispatchTable::Install(a, std::move(b)); });
    refused_ = !installed_;
}

title::MenuGuest MenuComposition::Guest() {
    title::MenuGuest g;
    g.read32 = [](uint32_t a) { return memory::read<uint32_t>(a); };
    g.write32 = [](uint32_t a, uint32_t v) { memory::write<uint32_t>(a, v); };
    g.write_bytes = [](uint32_t a, const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; ++i) memory::write<uint8_t>(a + static_cast<uint32_t>(i), b[i]);
    };
    g.read_string = [](uint32_t a, char* out, size_t max) {
        size_t n = 0;
        for (; n < max; ++n) {
            const uint8_t c = memory::read<uint8_t>(a + static_cast<uint32_t>(n));
            if (!c) break;
            out[n] = static_cast<char>(c);
        }
        out[n] = 0;
        return static_cast<uint32_t>(n);
    };
    g.call = [](CpuContext& ctx, uint32_t fn, const uint32_t* args, size_t count) {
        for (size_t i = 0; i < count && i < 8; ++i)   // a0-a3, t0-t3, sign-extended as the EE loads them
            ctx.cpuRegs.GPR.r[4 + i].SD[0] = static_cast<int32_t>(args[i]);
        DispatchTable::Invoke(fn, ctx);
        return ctx.cpuRegs.GPR.r[2].UL[0];
    };
    return g;
}

namespace {
using Action = SessionControls::Action;

title::MenuWidget Toggle(SessionRuntime& session, title::MenuStrings::Id label, const char* key,
                         std::function<bool(const SessionRuntime&)> read, Action action) {
    title::MenuWidget w;
    w.kind = title::MenuWidget::Kind::Toggle;
    w.label = label;
    w.key = key;
    w.read = [&session, read] { return read(session) ? 1 : 0; };
    w.apply = [&session, action](int32_t v) { session.Controls().Submit(action, v ? 1 : 0); };
    return w;
}

title::MenuWidget Enum(SessionRuntime& session, title::MenuStrings::Id label, const char* key,
                       std::vector<title::MenuStrings::Id> choices, std::function<int32_t(const SessionRuntime&)> read,
                       Action action) {
    title::MenuWidget w;
    w.kind = title::MenuWidget::Kind::Enum;
    w.label = label;
    w.key = key;
    w.choices = std::move(choices);
    w.minimum = 0;
    w.maximum = static_cast<int32_t>(w.choices.size()) - 1;
    w.read = [&session, read] { return read(session); };
    w.apply = [&session, action](int32_t v) { session.Controls().Submit(action, v); };
    return w;
}
}  // namespace

void MenuComposition::Declare(title::MenuExtension& extension, SessionRuntime& session) {
    title::MenuPageSpec display;
    display.title = title::MenuStrings::Id::Display;
    display.widgets.push_back(Toggle(session, title::MenuStrings::Id::PerfOverlay, "ui.perf_overlay",
                                     [](const SessionRuntime& s) { return s.PerfOverlay(); }, Action::SetPerfOverlay));
    using Id = title::MenuStrings::Id;
    // video.fullscreen: windowed / fullscreen / borderless (the mode's order).
    display.widgets.push_back(Enum(session, Id::DisplayMode, "video.fullscreen", {Id::Windowed, Id::Fullscreen, Id::Borderless},
                                   [](const SessionRuntime& s) { return s.DisplayModeState(); }, Action::SetDisplayMode));
    // video.scale: the choice index is the factor (0 = native window is the last choice).
    display.widgets.push_back(Enum(session, Id::Scale, "video.scale", {Id::ScaleNative, Id::Scale1x, Id::Scale2x, Id::Scale3x, Id::Scale4x},
                                   [](const SessionRuntime& s) { return s.ScaleSetting(); }, Action::SetScale));
    display.widgets.push_back(Toggle(session, Id::VSync, "video.vsync",
                                     [](const SessionRuntime& s) { return s.VSyncState(); }, Action::SetVSync));
    // video.aspect in the AspectSel order: auto, 4:3, 16:9, 16:10, 21:9.
    display.widgets.push_back(Enum(session, Id::Aspect, "video.aspect",
                                   {Id::AspectAuto, Id::AspectNormal, Id::AspectWide, Id::AspectWideTen, Id::AspectUltraWide},
                                   [](const SessionRuntime& s) { return s.AspectSetting(); }, Action::SetAspect));
    // video.render_rate (A3) is PARKED (2026-10-13): `display` causes the
    // pause->Continue ring-screen regression (s7 vs s8 bisect; 60 clean), so the row is
    // HIDDEN from this page (FrameReview never re-projects the
    // screen-space passes; pause->Continue pinned). The setting stays (default 60, the
    // ini and --set still accept it; MenuStrings keeps the row's strings); the pins
    // RootMenuPerfOverlay / RootDisplayRowsLive read items=8 and no video.render_rate key.
    // A2 texture filtering: the mode's choice index IS the setting's enum order
    // (title|point|bilinear|trilinear); anisotropy's choices are 1<<index.
    {
        title::MenuWidget filter;
        filter.kind = title::MenuWidget::Kind::Enum;
        filter.label = title::MenuStrings::Id::TextureFilter;
        filter.key = "video.texture_filter";
        filter.choices = {title::MenuStrings::Id::FilterTitle, title::MenuStrings::Id::FilterPoint,
                          title::MenuStrings::Id::FilterBilinear, title::MenuStrings::Id::FilterTrilinear};
        filter.read = [&session] { return session.TextureFilterState(); };
        filter.apply = [&session](int32_t v) { session.Controls().Submit(Action::SetTextureFilter, v); };
        display.widgets.push_back(std::move(filter));
        title::MenuWidget aniso;
        aniso.kind = title::MenuWidget::Kind::Enum;
        aniso.label = title::MenuStrings::Id::Anisotropy;
        aniso.key = "video.anisotropy";
        aniso.choices = {title::MenuStrings::Id::Aniso1, title::MenuStrings::Id::Aniso2, title::MenuStrings::Id::Aniso4,
                         title::MenuStrings::Id::Aniso8, title::MenuStrings::Id::Aniso16};
        aniso.read = [&session] {
            int32_t index = 0;
            for (int value = session.AnisotropyState(); value > 1; value >>= 1) ++index;
            return index;
        };
        aniso.apply = [&session](int32_t v) { session.Controls().Submit(Action::SetAnisotropy, 1 << v); };
        display.widgets.push_back(std::move(aniso));
    }
    extension.AddPage(std::move(display));
    title::MenuPageSpec controls;
    controls.title = title::MenuStrings::Id::Controls;
    controls.widgets.push_back(Toggle(session, title::MenuStrings::Id::KeyboardMouse, "input.kbm_pad",
                                      [](const SessionRuntime& s) { return s.KeyboardInputState(); }, Action::SetKeyboardInput));
    extension.AddPage(std::move(controls));
}

}  // namespace ps2r::app
