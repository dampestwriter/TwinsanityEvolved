#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_state.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_pass_fixture.h"

#include <cstdio>
#include <stdexcept>

namespace {
using F = ShadowPassFixture;
using L = title::ShadowLibrary;
using S = title::ShadowState;
using Blob = ps2r::savestate::Blob;
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
L::Snapshot Snapshot(const F& fixture) {
    L::Snapshot value;
    value.selected = true;
    CHECK(fixture.assets.Capture(value.assets) == title::ShadowAssets::Result::Ok);
    return value;
}
void Install(L& owner, F& fixture) {
    CHECK(owner.Install([&](uint32_t at) { return fixture.Capture(at); },
                        [&](uint32_t at, L::Body body) { return fixture.Install(at, std::move(body)); }));
}
void Pass(F& fixture) {
    fixture.Run(F::Source::kBegin);
    fixture.Mesh(0x1100, 0);
    fixture.Run(F::Source::kEnd);
    fixture.Run(F::Pass::kClose);
    fixture.Run(F::Pass::kForget);
}
void OwnerRestore() {
    F fixture(true, false);
    L owner(fixture.Hooks(true));
    const auto initial = Snapshot(fixture);
    Install(owner, fixture);
    CHECK(owner.Stage(initial, fixture.Reader()) && owner.Publish());
    CHECK(owner.Counts().restores == 1);
    Pass(fixture);
    CHECK(!owner.CanCheckpoint());
    L::Snapshot captured;
    CHECK(!owner.CaptureState(captured));
    CHECK(owner.Stage(initial, fixture.Reader()) && !owner.Publish());
    CHECK(fixture.batches.back()->Finish(true));
    CHECK(owner.Counts().passes.completed == 1 && owner.Counts().passes.mesh.replaced == 1);
    CHECK(owner.CaptureState(captured));
    CHECK(owner.Publish());
    CHECK(owner.Counts().passes.completed == 1 && owner.Counts().restores == 2);
    Pass(fixture); // same installed callbacks now resolve the new asset owner
    CHECK(fixture.batches.back()->Finish(true));
    CHECK(owner.Counts().passes.mesh.replaced == 2 && owner.Counts().passes.completed == 2);
    CHECK(owner.Counts().passes.mesh.model_calls == 0 && fixture.original_calls[F::Source::kMesh] == 0);
    // The empty census merges across the restore (the retired owner's frame and
    // the live owner's): run.meta graphics_shadow_empty read frames=0 while
    // empty=1710 when the merge dropped these fields.
    CHECK(owner.Counts().passes.frames_closed == 2 && owner.Counts().passes.frames_one_bracket == 2 &&
          owner.Counts().passes.empty_frames_total == 0 &&
          owner.Counts().passes.last_published_frame == fixture.batches.back()->frame.source_frame);
    CHECK(owner.CaptureState(captured));
    CHECK(captured.assets.assets.size() == 1 && captured.assets.assets[0].token.generation == 1);
    CHECK(captured.assets.next_generation == 2);
    CHECK(owner.Stage(captured, fixture.Reader()));
    fixture.Store(0x1004, 0x6bb7b765);
    CHECK(!owner.Publish()); // RAM changed after validation; live owner remains
    CHECK(owner.Counts().restores == 2);
    fixture.Store(0x1004, 0xd8d37a1c);
    CHECK(owner.Publish());
    auto malformed = captured;
    malformed.assets.assets[0].vertices = 4;
    CHECK(!owner.Stage(malformed, fixture.Reader()) && !owner.Publish());
    CHECK(owner.CaptureState(captured));
    CHECK(!owner.Stage({}, fixture.Reader())); // explicit model selection mismatch
    CHECK(owner.Counts().restores == 3);
}
std::vector<Blob> Image(const F& fixture, const L::Snapshot& snapshot) {
    std::vector<uint8_t> payload;
    CHECK(S::Encode(snapshot, payload));
    Blob ram;
    ram.tag = ps2r::savestate::TAG_RAM;
    ram.payload = fixture.Memory();
    ram.payload.resize(0x02000000);
    return {std::move(ram), {S::kTag, S::kVersion, snapshot.selected ? 1u : 0u, std::move(payload)}};
}
bool Preflight(const std::vector<Blob>& input, bool selected, L::Snapshot& snapshot) {
    // Every mutation in this test is re-serialized with valid section/file CRCs.
    // Semantic selection and identity checks must reject independently of CRC.
    const auto bytes = ps2r::savestate::SerializeStateFile({}, input);
    std::vector<Blob> parsed;
    ps2r::savestate::FileMeta meta;
    std::string error;
    CHECK(ps2r::savestate::ParseStateFile(bytes.data(), bytes.size(), &meta, &parsed, &error));
    title::MaterialState::Read incoming;
    return S::Preflight(parsed, selected, snapshot, incoming, &error);
}
void Selection() {
    F fixture(true, false);
    const auto original = Snapshot(fixture);
    auto image = Image(fixture, original);
    L::Snapshot decoded;
    CHECK(Preflight(image, true, decoded) && decoded.selected);
    CHECK(decoded.assets.assets[0].bytes == original.assets.assets[0].bytes);
    CHECK(!Preflight(image, false, decoded));
    image.back().flags = 0;
    CHECK(!Preflight(image, true, decoded));
    image.back().flags = 1;
    image.back().version = 2;
    CHECK(!Preflight(image, true, decoded));
    image.back().version = 1;
    image[0].payload[0x1004] ^= 1;
    CHECK(!Preflight(image, true, decoded));
    image[0].payload[0x1004] ^= 1;
    auto missing = original;
    missing.assets.assets.clear();
    CHECK(S::Encode(missing, image.back().payload));
    CHECK(!Preflight(image, true, decoded)); // source shape still needs its omitted asset
    CHECK(S::Encode(original, image.back().payload));
    image[0].payload[0x1110] = 1;
    CHECK(!Preflight(image, true, decoded)); // active old-model instance block
    image[0].payload[0x1110] = 0;
    image[0].payload[0x6028] = 1;
    CHECK(!Preflight(image, true, decoded)); // foreign material writer
    image[0].payload[0x6028] = 0;
    CHECK(Preflight(image, true, decoded));
    image.push_back(image.back());
    CHECK(!Preflight(image, true, decoded));
    image.pop_back();
    image.pop_back();
    CHECK(!Preflight(image, true, decoded));
    CHECK(Preflight(image, false, decoded) && !decoded.selected); // known old model owner only
    image = Image(fixture, {});
    CHECK(Preflight(image, false, decoded));
    CHECK(!Preflight(image, true, decoded));
    image.erase(image.begin());
    CHECK(!Preflight(image, false, decoded));
}
void Codec() {
    F fixture(true, false);
    const auto initial = Snapshot(fixture);
    std::vector<uint8_t> encoded;
    CHECK(S::Encode(initial, encoded));
    for (size_t size = 0; size < encoded.size(); ++size) {
        L::Snapshot output;
        output.assets.next_generation = 99;
        CHECK(!S::Decode({encoded.begin(), encoded.begin() + size}, output));
        CHECK(output.assets.next_generation == 99); // atomic rejection
    }
    auto bytes = encoded;
    bytes.push_back(0);
    L::Snapshot output;
    CHECK(!S::Decode(bytes, output));
    for (size_t at : {0u, 1u, 2u, 3u}) {
        bytes = encoded;
        bytes[at] = 2;
        CHECK(!S::Decode(bytes, output));
    }
    CHECK(S::Decode(encoded, output));
    std::vector<uint8_t> again;
    CHECK(S::Encode(output, again) && again == encoded);
    auto malformed = initial;
    malformed.selected = false;
    CHECK(!S::Encode(malformed, again) && again == encoded);
}
void InstallRollback() {
    for (unsigned fail = 1; fail <= 7; ++fail) {
        F fixture(true, false);
        auto owner = std::make_unique<L>(fixture.Hooks(true));
        unsigned calls = 0;
        CHECK(!owner->Install([&](uint32_t at) { return fixture.Capture(at); },
            [&](uint32_t at, L::Body body) {
                fixture.Install(at, std::move(body));
                return ++calls != fail;
            }));
        owner.reset();
        fixture.Run(F::Source::kBegin); fixture.Run(F::Source::kEnd);
        CHECK(fixture.original_calls[F::Source::kBegin] == 1 &&
              fixture.original_calls[F::Source::kEnd] == 1 && fixture.batches.empty());
    }
    F fixture(false, false);
    L owner(fixture.Hooks(false));
    Install(owner, fixture);
    Pass(fixture);
    CHECK(owner.Counts().passes.mesh.model_calls == 1 && fixture.batches.empty());
    CHECK(owner.Stage({}, fixture.Reader()) && owner.Publish());
    Pass(fixture);
    CHECK(owner.Counts().passes.mesh.model_calls == 2 && owner.Counts().restores == 1);
}
void SourceReadBoundary() {
    F fixture(true, false);
    L owner(fixture.Hooks(true));
    bool fail = false;
    fixture.Install(title::ShadowAssets::kRead, [&](CpuContext&, uint32_t) {
        CHECK(!owner.CanCheckpoint()); // the preserved Stream read can yield here
        L::Snapshot value;
        CHECK(!owner.CaptureState(value));
        if (fail) throw std::runtime_error("source stream failure");
    });
    Install(owner, fixture);
    fixture.Store(0x2004, 42); // a source model outside the shadow family
    CpuContext cpu{};
    cpu.cpuRegs.GPR.r[4].UL[0] = 0x2000;
    fixture.Capture(title::ShadowAssets::kRead)(cpu, 0);
    CHECK(owner.CanCheckpoint() && owner.Counts().loads.unselected == 1);
    fail = true;
    bool threw = false;
    try { fixture.Capture(title::ShadowAssets::kRead)(cpu, 0); } catch (...) { threw = true; }
    CHECK(threw && owner.Counts().loads.failed == 1 && !owner.CanCheckpoint());
}
}
int main() {
    try {
        OwnerRestore(); Selection(); Codec(); InstallRollback(); SourceReadBoundary();
        std::printf("ShadowState: %u generation, live binding, CRC-valid selection and refusal checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
