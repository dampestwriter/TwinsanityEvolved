#include "games/slus_20909_crash_twinsanity/composition/effects_application.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/mem/main_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_source.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_state.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_pass_fixture.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

// The root-facing contract of the effects composition exactly as the
// bootstrap phase TUs consume it (integration/distortion_application.patch):
// the default-off arm the 13x3 battery runs, with a dispatch stand-in whose
// Capture returns the CURRENTLY installed body (DispatchTable::Capture), so
// the ForgetMaterials entry both owners install chains to one original.
namespace {
using App = ps2r::app::EffectsApplication;
using F = DistortionPassFixture;
using P = title::DistortionPass;
using Shadows = title::ShadowPass;
using Source = title::ShadowSource;
using Screen = title::ScreenPass;
using Blob = ps2r::savestate::Blob;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint64_t Settings = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 | uint64_t{1} << 56 | uint64_t{1} << 59;

struct Dispatch {
    std::map<uint32_t, App::Body> bodies;       // what is dispatched now
    std::map<uint32_t, unsigned> originals;     // generated-body executions
    std::map<uint32_t, unsigned> installs;      // Install calls per address
    std::map<uint32_t, bool> selected;          // the installer's third argument
};
App::Capture Capture(Dispatch& d) {
    return [&d](uint32_t at) -> App::Body {
        const auto found = d.bodies.find(at);
        if (found != d.bodies.end()) return found->second;
        return [&d, at](CpuContext&, uint32_t) { ++d.originals[at]; };
    };
}
App::Installer Install(Dispatch& d) {
    return [&d](uint32_t at, App::Body body, bool is_selected) {
        d.bodies[at] = std::move(body);
        ++d.installs[at];
        d.selected[at] = is_selected;
        return true;
    };
}
void Run(Dispatch& d, uint32_t at, CpuContext context, uint32_t entry) { d.bodies.at(at)(context, entry); }
void Prepare(MainMemory& memory) {
    F::Write<uint32_t>(title::DistortionMaterial::kVtable + 0x2c, title::DistortionMaterial::kSettings);
    F::Write<uint64_t>(F::Shader, Settings);
    std::vector<uint8_t> bytes(0x400000);
    CHECK(F::Reader(0, bytes.data(), bytes.size()));
    std::memcpy(memory.Data(), bytes.data(), bytes.size());
}
Blob Ram(const MainMemory& memory) {
    Blob ram;
    ram.tag = ps2r::savestate::TAG_RAM;
    ram.version = 1;
    ram.payload.assign(memory.Data(), memory.Data() + memory.Size());
    return ram;
}

void ControlArmComposition() {
    F fixture(false);
    MainMemory memory;
    Prepare(memory);
    Dispatch dispatch;
    // The root passes a null video stack when nothing is selected and the
    // same four services it gives the selected arm; the environment service
    // is the empty one the patch documents.
    unsigned captures = 0, reserves = 0, fences = 0, checkpoints = 0;
    App::Services services;
    services.capture_scene = [&](uint32_t, auto&) { ++captures; return false; };
    services.reserve = [&](uint32_t, App::Work) { ++reserves; return false; };
    services.ordered = [&](App::Work work) { ++fences; return work(); };
    services.checkpoint = [&](std::string*) { ++checkpoints; return true; };
    App app(memory, nullptr, App::Selection{}, services);
    CHECK(!app.Selected());
    CHECK(!app.Install({}, Install(dispatch)));
    CHECK(!app.Install(Capture(dispatch), {}));
    CHECK(dispatch.bodies.empty());
    CHECK(app.Install(Capture(dispatch), Install(dispatch)));
    // Seven distinct generated entries: the shadow producer's five, the
    // distortion producer's two sharing ForgetMaterials 0x1c0410, and the
    // bucket-4 DrawScreenEffects boundary.
    const std::set<uint32_t> expected{Source::kBegin, Source::kMesh, Source::kEnd, Shadows::kClose,
                                      Shadows::kForget, P::kDraw, Screen::kDraw};
    std::set<uint32_t> installed;
    for (const auto& [at, body] : dispatch.bodies) installed.insert(at);
    CHECK(installed == expected && Shadows::kForget == P::kForget);
    CHECK(dispatch.installs.at(P::kForget) == 2);
    for (const auto& [at, count] : dispatch.installs) CHECK(count == (at == P::kForget ? 2u : 1u));
    for (const auto& [at, s] : dispatch.selected) CHECK(!s); // every body is the root's DelegateInterior candidate
    // The shared Forget entry chains distortion -> shadows -> the one original.
    Run(dispatch, P::kForget, F::Context(0), P::kForget);
    CHECK(dispatch.originals[P::kForget] == 1);
    // State: both sections register through one preflight installer; the
    // model arm can checkpoint only once registered.
    CHECK(!app.CanCheckpoint());
    ps2r::savestate::SectionRegistry registry;
    std::vector<App::Preflight> preflights;
    CHECK(app.Register(registry, [&](App::Preflight hook) { preflights.push_back(std::move(hook)); }));
    CHECK(preflights.size() == 2);
    CHECK(registry.Has(title::ShadowState::kTag) && registry.Has(title::DistortionState::kTag));
    CHECK(app.CanCheckpoint());
    // One quiescence condition per family, under the names the parent's
    // census and the former hand-registered "graphics_shadows" used.
    std::map<std::string, std::function<bool(std::string*)>> conditions;
    app.RegisterConditions([&](const char* name, auto condition) { conditions[name] = std::move(condition); });
    CHECK(conditions.size() == 3 && conditions.count("graphics_shadows") && conditions.count("graphics_distortion") &&
          conditions.count("graphics_screen"));
    std::string error;
    for (auto& [name, condition] : conditions) CHECK(condition(&error));
    // Both model selections record; the captured image is model-only.
    const auto captured = registry.Capture();
    CHECK(captured.size() == 2);
    for (const auto& section : captured) CHECK(section.flags == 0);
    const Blob ram = Ram(memory);
    for (auto& preflight : preflights) CHECK(preflight({ram}, &error));
    CHECK(registry.Restore({}, &error));
    // The model distortion draw executes the generated original and counts it.
    Run(dispatch, P::kDraw, F::Context(F::Block0), P::kDraw);
    CHECK(dispatch.originals[P::kDraw] == 1);
    ps2r::verify::RunMeta meta("", {});
    app.Record(meta);
    const auto text = meta.Render();
    CHECK(text.find("graphics_shadow_selection=model") != std::string::npos);
    CHECK(text.find("graphics_distortion_selection=model") != std::string::npos);
    CHECK(text.find("graphics_distortion_draw=calls=1,replaced=0,model_calls=1,interior=0") != std::string::npos);
    CHECK(text.find("graphics_shadow_begin=calls=0,replaced=0,model_calls=0,interior=0") != std::string::npos);
    // The screen boundary forwards whole and counts; a context without the
    // title's $gp is unreadable, never a refusal of the model call.
    Run(dispatch, Screen::kDraw, F::Context(0), Screen::kDraw);
    CHECK(dispatch.originals[Screen::kDraw] == 1);
    ps2r::verify::RunMeta after("", {});
    app.Record(after);
    const auto rendered = after.Render();
    CHECK(rendered.find("graphics_screen_selection=model") != std::string::npos);
    CHECK(rendered.find("graphics_screen_effects=calls=1,empty=0,effects=0,blur=0,filter=0,wave=0,model_calls=1,replaced=0,"
                        "filter_off=0,refused=0,last_refusal=none,faults=0,last_palette=0,last_blends=0,last_height=0,interior=0,"
                        "unreadable=1") != std::string::npos);
    CHECK(rendered.find("graphics_screen_scene=passes=0,filter_draws=0,blur_passes=0,palette_uploads=0,wave_passes=0,"
                        "allocations=0,failures=0") != std::string::npos);
    // The control arm never touched the scene services.
    CHECK(captures == 0 && reserves == 0 && fences == 0);
    CHECK(app.StopAfterQuiescence(&error) && app.StopAfterQuiescence());
}

void SelectionRequiresLiveVideo() {
    MainMemory memory;
    App::Services services;
    services.capture_scene = [](uint32_t, auto&) { return false; };
    services.reserve = [](uint32_t, App::Work) { return false; };
    services.ordered = [](App::Work work) { return work(); };
    services.checkpoint = [](std::string*) { return true; };
    for (const auto selection : {App::Selection{true, false, false}, App::Selection{false, true, false},
                                 App::Selection{true, true, false}, App::Selection{false, false, true}}) {
        bool threw = false;
        try { App selected(memory, nullptr, selection, services); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw); // no silently successful selected owner without the shared Graphics owner
    }
    // Nothing selected tolerates missing services too: the root composes the
    // same object on the software backend where no video stack exists.
    App bare(memory, nullptr, App::Selection{}, {});
    CHECK(!bare.Selected());
    Dispatch dispatch;
    CHECK(bare.Install(Capture(dispatch), Install(dispatch)));
    CHECK(dispatch.bodies.size() == 7);
    CHECK(bare.StopAfterQuiescence());
}
}  // namespace

int main() {
    try {
        ControlArmComposition(); SelectionRequiresLiveVideo();
        std::printf("EffectsApplication: %u root composition checks (control arm, shared Forget chain, conditions)\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
