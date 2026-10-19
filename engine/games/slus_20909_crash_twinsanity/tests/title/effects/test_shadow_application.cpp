#include "games/slus_20909_crash_twinsanity/composition/shadow_application.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/mem/main_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_state.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_pass_fixture.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using App = ps2r::app::ShadowApplication;
using F = ShadowPassFixture;
using Codec = title::ShadowState;
using Blob = ps2r::savestate::Blob;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
void SelectedComposition() {
    F fixture(true, false);
    EffectsFixture world;
    MainMemory memory;
    std::memcpy(memory.Data(), fixture.Memory().data(), fixture.Memory().size());
    std::vector<App::Work> queue;
    bool scene_ready = true, ordered_ready = true;
    unsigned fences = 0;
    App::Output output;
    output.frame = [&](auto& frame) { frame = world.frame; return true; };
    output.reserve = [&](auto work) { queue.push_back(std::move(work)); return true; };
    output.ordered = [&](auto work) { ++fences; return ordered_ready && work(); };
    output.checkpoint = [&](auto* error) {
        if (!scene_ready && error) *error = "shared surface capture is not ready";
        return scene_ready;
    };
    App app(memory, &world.effects, true, std::move(output));
    CHECK(app.Install([&](uint32_t at) { return fixture.Capture(at); },
                      [&](uint32_t at, App::Body body) { return fixture.Install(at, std::move(body)); }));
    CHECK(!app.CanCheckpoint()); // state registration is a real dependency
    ps2r::savestate::SectionRegistry registry;
    App::Preflight preflight;
    CHECK(app.Register(registry, [&](App::Preflight hook) { preflight = std::move(hook); }));
    CHECK(registry.Has(Codec::kTag) && app.CanCheckpoint());
    title::ShadowLibrary::Snapshot initial;
    initial.selected = true;
    CHECK(fixture.assets.Capture(initial.assets) == title::ShadowAssets::Result::Ok);
    std::vector<uint8_t> payload;
    CHECK(Codec::Encode(initial, payload));
    Blob ram;
    ram.tag = ps2r::savestate::TAG_RAM;
    ram.payload.assign(memory.Data(), memory.Data() + memory.Size());
    std::vector<Blob> sections{ram, {Codec::kTag, Codec::kVersion, 1, payload}};
    std::string error;
    CHECK(!registry.Restore({sections[1]}, &error)); // no staged incoming image
    CHECK(preflight(sections, &error));
    CHECK(registry.Restore({sections[1]}, &error));
    CHECK(fences == 1 && app.Counts().restores == 1);
    const auto drive = [&] {
        fixture.Run(F::Source::kBegin);
        CpuContext cpu{};
        cpu.cpuRegs.GPR.r[4].UL[0] = 0x1100;
        cpu.cpuRegs.GPR.r[5].UL[0] = F::Source::kShadowScreen;
        cpu.cpuRegs.GPR.r[6].UL[0] = F::Source::kShadowCamera;
        fixture.Capture(F::Source::kMesh)(cpu, 0);
        fixture.Run(F::Source::kEnd);
        fixture.Run(F::Pass::kClose);
        fixture.Run(F::Pass::kForget);
    };
    CHECK(world.pipeline.DrawGeneric(world.OrdinaryDraw()).drew);
    drive();
    CHECK(queue.size() == 1 && !app.CanCheckpoint());
    CHECK(preflight(sections, &error));
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 1);
    CHECK(queue.back()());
    queue.clear();
    CHECK(app.CanCheckpoint() && app.Counts().passes.completed == 1);
    CHECK(world.effects.Counts().shadow_passes == 1 && world.effects.ShadowCounts().strips == 1);
    CHECK(world.backend.uploads == 0 && world.backend.readbacks == 0);
    const auto captured = registry.Capture();
    CHECK(captured.size() == 1 && captured[0].flags == 1 && captured[0].payload == payload);
    ordered_ready = false;
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 1);
    ordered_ready = true;
    CHECK(registry.Restore({sections[1]}, &error));
    CHECK(app.Counts().restores == 2 && app.Counts().passes.completed == 1);
    drive();
    CHECK(queue.back()());
    queue.clear();
    CHECK(app.Counts().passes.completed == 2 && app.Counts().passes.mesh.replaced == 2);
    CHECK(fixture.original_calls[F::Source::kBegin] == 0 && fixture.original_calls[F::Source::kMesh] == 0 &&
          fixture.original_calls[F::Source::kEnd] == 0);
    ps2r::verify::RunMeta meta("", {});
    app.Record(meta);
    const auto text = meta.Render();
    CHECK(text.find("graphics_shadow_mesh=calls=2,replaced=2,model_calls=0,interior=0") != std::string::npos);
    CHECK(text.find("instances=2,blocks=2,groups=2,completed=2,empty=0,faults=0,refused=0,limit_refusals=0,refused_meshes=0,last_mesh_refusal=none,restores=2") != std::string::npos);
    scene_ready = false;
    CHECK(!app.CanCheckpoint(&error));
    bool threw = false;
    try { registry.Capture(); } catch (...) { threw = true; }
    CHECK(threw);
    scene_ready = true;
    CHECK(preflight(sections, &error));
    CHECK(!preflight({ram}, &error)); // failed new attempt invalidates prior prepared work
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 2);
    CHECK(app.CanCheckpoint());
    drive(); // shutdown explicitly abandons an unsubmitted source frame
    CHECK(queue.size() == 1);
    CHECK(app.StopAfterQuiescence(&error));
    CHECK(!queue.back()()); // a stale callback cannot access stopped resources
    queue.clear();
    CHECK(app.Counts().passes.completed == 2 && !app.CanCheckpoint());
    CHECK(!preflight(sections, &error) && !registry.Restore({sections[1]}, &error));
    const auto stopped_fences = fences;
    CHECK(app.StopAfterQuiescence() && fences == stopped_fences);
}
void ModelAndMissingOwner() {
    F fixture(false, false);
    MainMemory memory;
    std::memcpy(memory.Data(), fixture.Memory().data(), fixture.Memory().size());
    App app(memory, nullptr, false, {});
    CHECK(app.Install([&](uint32_t at) { return fixture.Capture(at); },
                      [&](uint32_t at, App::Body body) { return fixture.Install(at, std::move(body)); }));
    ps2r::savestate::SectionRegistry registry;
    App::Preflight preflight;
    CHECK(app.Register(registry, [&](App::Preflight hook) { preflight = std::move(hook); }));
    const auto captured = registry.Capture();
    CHECK(captured.size() == 1 && captured[0].flags == 0);
    Blob ram;
    ram.tag = ps2r::savestate::TAG_RAM;
    ram.payload.assign(memory.Data(), memory.Data() + memory.Size());
    std::string error;
    bool threw = false;
    try { registry.Restore({}, &error); } catch (...) { threw = true; }
    CHECK(threw); // absence alone is never a reset/import proof
    CHECK(preflight({ram}, &error) && registry.Restore({}, &error));
    CHECK(app.Counts().restores == 1);
    fixture.Run(F::Source::kBegin);
    fixture.Run(F::Source::kEnd);
    CHECK(app.Counts().passes.begin.model_calls == 1 && app.Counts().passes.end.model_calls == 1);
    auto malformed = captured[0];
    malformed.payload[1] = 1;
    CHECK(!preflight({ram, malformed}, &error));
    threw = false;
    try { App missing(memory, nullptr, true, {}); } catch (...) { threw = true; }
    CHECK(threw); // no silently successful owner without scene/queue/persistence
}
}
int main() {
    try {
        SelectedComposition(); ModelAndMissingOwner();
        std::printf("ShadowApplication: %u live source/scene/restore/census checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
