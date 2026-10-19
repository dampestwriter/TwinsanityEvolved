#include "games/slus_20909_crash_twinsanity/composition/distortion_application.h"
#include "engine/core/verify/run_meta.h"
#include "engine/ee/mem/main_memory.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_state.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_pass_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

// The composed distortion owner over real MainMemory, a real EffectsScene on
// the fake backend, a dispatch-table stand-in and the savestate registry.
namespace {
using App = ps2r::app::DistortionApplication;
using F = DistortionPassFixture;
using P = title::DistortionPass;
using Codec = title::DistortionState;
using Blob = ps2r::savestate::Blob;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint64_t Settings = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 | uint64_t{1} << 56 | uint64_t{1} << 59;

struct Dispatch {
    std::map<uint32_t, App::Body> bodies;
    std::map<uint32_t, unsigned> originals;
};
App::Capture Capture(Dispatch& d) {
    return [&d](uint32_t at) { return App::Body([&d, at](CpuContext&, uint32_t) { ++d.originals[at]; }); };
}
App::Installer Install(Dispatch& d) {
    return [&d](uint32_t at, App::Body body) { d.bodies[at] = std::move(body); return true; };
}
void Run(Dispatch& d, uint32_t at, CpuContext context, uint32_t entry) { d.bodies.at(at)(context, entry); }
void Prepare(MainMemory& memory) {
    // The retail vtable's settings slot, which the fixture's flat RAM lacks.
    F::Write<uint32_t>(title::DistortionMaterial::kVtable + 0x2c, title::DistortionMaterial::kSettings);
    F::Write<uint64_t>(F::Shader, Settings); // the object's attributes, as the constructor left them
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

void SelectedComposition() {
    F fixture(true);
    EffectsFixture world;
    MainMemory memory;
    Prepare(memory);
    Dispatch dispatch;
    std::vector<App::Work> queue;
    bool scene_ready = true, ordered_ready = true;
    unsigned fences = 0;
    App::Output output;
    output.environment = [&](auto& environment) { environment = fixture.source.environment; return true; };
    output.frame = [&](auto& frame) { frame = world.frame; return true; };
    output.reserve = [&](auto work) { queue.push_back(std::move(work)); return true; };
    output.ordered = [&](auto work) { ++fences; return ordered_ready && work(); };
    output.checkpoint = [&](auto* error) {
        if (!scene_ready && error) *error = "shared surface capture is not ready";
        return scene_ready;
    };
    App app(memory, &world.effects, true, std::move(output));
    CHECK(app.Install(Capture(dispatch), Install(dispatch)));
    CHECK(dispatch.bodies.size() == 6); // three block observers, the settings call, draw, forget
    CHECK(!app.CanCheckpoint()); // state registration is a real dependency
    ps2r::savestate::SectionRegistry registry;
    App::Preflight preflight;
    CHECK(app.Register(registry, [&](App::Preflight hook) { preflight = std::move(hook); }));
    CHECK(registry.Has(Codec::kTag) && app.CanCheckpoint());
    // A fresh owner has observed no settings version: the first frame is
    // refused to the model (the generated draw runs, nothing is reserved, no
    // fault) until the source forgets it.
    Run(dispatch, P::kDraw, F::Context(F::Block0), P::kDraw);
    CHECK(dispatch.originals[P::kDraw] == 1 && app.Counts().pass.refused == 1 && app.Counts().pass.model_calls == 1);
    CHECK(app.Counts().pass.faults == 0 && queue.empty() && !app.CanCheckpoint());
    Run(dispatch, P::kForget, F::Context(0), P::kForget);
    CHECK(app.CanCheckpoint() && dispatch.originals[P::kForget] == 1);
    // Restore the owned value state (block ownership + settings version) the
    // game would have produced at its initialisation and settings calls.
    title::DistortionLibrary::Snapshot initial;
    initial.selected = true;
    initial.blocks.generation = 1; initial.blocks.pool = 0x210000;
    initial.blocks.next = {{F::Block0, F::Block1}, {F::Block1, 0}, {F::Block2, 0}};
    initial.settings = {1, F::Shader, 1, {Settings, 0, 0}};
    std::vector<uint8_t> payload;
    CHECK(Codec::Encode(initial, payload));
    const Blob ram = Ram(memory);
    std::vector<Blob> sections{ram, {Codec::kTag, Codec::kVersion, 1, payload}};
    std::string error;
    CHECK(!registry.Restore({sections[1]}, &error)); // no staged incoming image
    CHECK(preflight(sections, &error)); // staging is pure
    CHECK(registry.Restore({sections[1]}, &error) && app.Counts().restores == 1);
    // With the version restored the draw is owned; an unowned block chain after
    // the reservation is a fault, and a faulted producer never publishes.
    bool faulted = false;
    try { Run(dispatch, P::kDraw, F::Context(0x201000), P::kDraw); } catch (const std::runtime_error&) { faulted = true; }
    CHECK(faulted && app.Counts().pass.faults == 1 && !app.CanCheckpoint() && queue.size() == 1);
    CHECK(preflight(sections, &error));
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 1);
    CHECK(app.StopAfterQuiescence(&error) && !app.CanCheckpoint());
    CHECK(!queue.back()()); // the stale reservation cannot use stopped resources
}

void RestoredSelectedComposition() {
    F fixture(true);
    EffectsFixture world;
    MainMemory memory;
    Prepare(memory);
    Dispatch dispatch;
    std::vector<App::Work> queue;
    bool scene_ready = true, ordered_ready = true;
    unsigned fences = 0;
    App::Output output;
    output.environment = [&](auto& environment) { environment = fixture.source.environment; return true; };
    output.frame = [&](auto& frame) { frame = world.frame; return true; };
    output.reserve = [&](auto work) { queue.push_back(std::move(work)); return true; };
    output.ordered = [&](auto work) { ++fences; return ordered_ready && work(); };
    output.checkpoint = [&](auto* error) {
        if (!scene_ready && error) *error = "shared surface capture is not ready";
        return scene_ready;
    };
    App app(memory, &world.effects, true, std::move(output));
    CHECK(app.Install(Capture(dispatch), Install(dispatch)));
    ps2r::savestate::SectionRegistry registry;
    App::Preflight preflight;
    CHECK(app.Register(registry, [&](App::Preflight hook) { preflight = std::move(hook); }));
    title::DistortionLibrary::Snapshot initial;
    initial.selected = true;
    initial.blocks.generation = 1; initial.blocks.pool = 0x210000;
    initial.blocks.next = {{F::Block0, F::Block1}, {F::Block1, 0}, {F::Block2, 0}};
    initial.settings = {1, F::Shader, 1, {Settings, 0, 0}};
    std::vector<uint8_t> payload;
    CHECK(Codec::Encode(initial, payload));
    const Blob ram = Ram(memory);
    std::vector<Blob> sections{ram, {Codec::kTag, Codec::kVersion, 1, payload}};
    std::string error;
    CHECK(preflight(sections, &error));
    CHECK(registry.Restore({sections[1]}, &error));
    CHECK(fences == 1 && app.Counts().restores == 1);
    const auto drive = [&] {
        Run(dispatch, P::kDraw, F::Context(F::Block0), P::kDraw);
        Run(dispatch, P::kDraw, F::Context(F::Block2), P::kDraw);
        Run(dispatch, P::kForget, F::Context(0), P::kForget);
    };
    CHECK(world.pipeline.DrawGeneric(world.OrdinaryDraw()).drew);
    drive();
    CHECK(queue.size() == 1 && !app.CanCheckpoint());
    CHECK(preflight(sections, &error));
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 1); // pending output
    CHECK(queue.back()());
    queue.clear();
    CHECK(app.CanCheckpoint() && app.Counts().pass.completed == 1 && app.Counts().pass.replaced == 2);
    CHECK(app.Counts().pass.blocks == 3 && app.Counts().pass.draws == 12);
    CHECK(world.effects.Counts().distortion_passes == 1 && world.effects.DistortionCounts().copies == 1 &&
          world.effects.DistortionCounts().batches == 12);
    CHECK(world.backend.readbacks == 0);
    CHECK(dispatch.originals[P::kDraw] == 0 && dispatch.originals[P::kForget] == 1); // forget forwards once
    const auto captured = registry.Capture();
    CHECK(captured.size() == 1 && captured[0].flags == 1 && captured[0].payload == payload);
    // The source settings call advances the version; the captured state follows.
    Run(dispatch, title::DistortionSettings::kCall, F::Context(F::Shader), title::DistortionSettings::kCall);
    CHECK(dispatch.originals[title::DistortionSettings::kCall] == 1 && app.Counts().settings.captured == 1);
    title::DistortionLibrary::Snapshot advanced;
    CHECK(Codec::Decode(registry.Capture()[0].payload, advanced) && advanced.settings.generation == 2 &&
          advanced.settings.shader == F::Shader && advanced.blocks.next.size() == 3);
    drive();
    CHECK(queue.back()());
    queue.clear();
    CHECK(app.Counts().pass.completed == 2 && app.Counts().pass.replaced == 4);
    ordered_ready = false;
    CHECK(preflight(sections, &error));
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 1);
    ordered_ready = true;
    CHECK(preflight(sections, &error) && registry.Restore({sections[1]}, &error));
    CHECK(app.Counts().restores == 2 && app.Counts().pass.completed == 2);
    ps2r::verify::RunMeta meta("", {});
    app.Record(meta);
    const auto text = meta.Render();
    CHECK(text.find("graphics_distortion_selection=owned") != std::string::npos);
    CHECK(text.find("graphics_distortion_draw=calls=4,replaced=4,model_calls=0,interior=0") != std::string::npos);
    CHECK(text.find("batches=2,blocks=6,draws=24,forgets=2,completed=2,faults=0,refused=0,restores=2") != std::string::npos);
    CHECK(text.find("graphics_distortion_settings=calls=1,captured=1,rejected=0,interior=0") != std::string::npos);
    scene_ready = false;
    CHECK(!app.CanCheckpoint(&error));
    bool threw = false;
    try { registry.Capture(); } catch (...) { threw = true; }
    CHECK(threw);
    scene_ready = true;
    // A selected image without its section or with foreign values is refused.
    CHECK(!preflight({ram}, &error));
    auto foreign = sections;
    foreign[1].payload[4] ^= 1; // pool
    CHECK(!preflight(foreign, &error));
    auto unversioned = initial;
    unversioned.settings.shader = F::Shader + 0x100; // no retail object there
    std::vector<uint8_t> unversioned_payload;
    CHECK(Codec::Encode(unversioned, unversioned_payload));
    CHECK(!preflight({ram, {Codec::kTag, Codec::kVersion, 1, unversioned_payload}}, &error));
    CHECK(!registry.Restore({sections[1]}, &error) && app.Counts().restores == 2);
    drive(); // shutdown explicitly abandons an unsubmitted source frame
    CHECK(queue.size() == 1);
    CHECK(app.StopAfterQuiescence(&error));
    CHECK(!queue.back()()); // a stale callback cannot access stopped resources
    queue.clear();
    CHECK(app.Counts().pass.completed == 2 && !app.CanCheckpoint());
    CHECK(!preflight(sections, &error) && !registry.Restore({sections[1]}, &error));
    const auto stopped_fences = fences;
    CHECK(app.StopAfterQuiescence() && fences == stopped_fences);
}

void ModelAndMissingOwner() {
    F fixture(false);
    MainMemory memory;
    Prepare(memory);
    Dispatch dispatch;
    App app(memory, nullptr, false, {});
    CHECK(app.Install(Capture(dispatch), Install(dispatch)));
    CHECK(dispatch.bodies.size() == 2); // the model arm installs only the producer's two entries
    ps2r::savestate::SectionRegistry registry;
    App::Preflight preflight;
    CHECK(app.Register(registry, [&](App::Preflight hook) { preflight = std::move(hook); }));
    const auto captured = registry.Capture();
    CHECK(captured.size() == 1 && captured[0].flags == 0 && captured[0].payload.size() == 4);
    const Blob ram = Ram(memory);
    std::string error;
    bool threw = false;
    try { registry.Restore({}, &error); } catch (...) { threw = true; }
    CHECK(threw); // absence alone is never a reset/import proof
    CHECK(preflight({ram}, &error) && registry.Restore({}, &error));
    CHECK(app.Counts().restores == 1);
    Run(dispatch, P::kDraw, F::Context(F::Block0), P::kDraw);
    Run(dispatch, P::kForget, F::Context(0), P::kForget);
    CHECK(app.Counts().pass.model_calls == 1 && app.Counts().pass.calls == 1 && app.Counts().pass.forgets == 0);
    CHECK(dispatch.originals[P::kDraw] == 1 && dispatch.originals[P::kForget] == 1);
    auto selected = captured[0];
    selected.payload[1] = 1;
    CHECK(!preflight({ram, selected}, &error));
    ps2r::verify::RunMeta meta("", {});
    app.Record(meta);
    CHECK(meta.Render().find("graphics_distortion_selection=model") != std::string::npos);
    threw = false;
    try { App missing(memory, nullptr, true, {}); } catch (...) { threw = true; }
    CHECK(threw); // no silently successful owner without scene/queue/persistence
}
}
int main() {
    try {
        SelectedComposition(); RestoredSelectedComposition(); ModelAndMissingOwner();
        std::printf("DistortionApplication: %u live source/scene/restore/census checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
