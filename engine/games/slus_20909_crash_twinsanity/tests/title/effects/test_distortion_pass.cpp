#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_pass.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_geometry.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_pass_fixture.h"

namespace {
using P = title::DistortionPass;
using G = title::DistortionGeometry;
using Fixture = DistortionPassFixture;
constexpr uint32_t Shader = Fixture::Shader, Block0 = Fixture::Block0, Block2 = Fixture::Block2;
template<class T> void Write(uint32_t at, const T& value) { Fixture::Write(at, value); }
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion pass line ") + std::to_string(__LINE__) + ": " + #value); } while (0)

std::vector<ps2r::gs::DrawIr> Expected(const Fixture& f, unsigned blocks) {
    std::vector<G::Block> chain(blocks, f.source.blocks[0]);
    std::vector<ps2r::gs::DrawIr> out;
    CHECK(G::Build(chain, f.source.look, f.source.view, f.source.material, out) == G::Result::Ok);
    return out;
}
bool Same(const std::vector<ps2r::gs::DrawIr>& a, const std::vector<ps2r::gs::DrawIr>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].verts.size() != b[i].verts.size() || a[i].snapshot.vertex_hash != b[i].snapshot.vertex_hash ||
            a[i].tex.tbp != b[i].tex.tbp || a[i].src_frame != b[i].src_frame || a[i].raster.alpha.a != b[i].raster.alpha.a)
            return false;
        if (std::memcmp(a[i].verts.data(), b[i].verts.data(), a[i].verts.size() * sizeof(a[i].verts[0])) != 0)
            return false;
    }
    return true;
}

void OrderedFrames() {
    Fixture f; auto pass = f.Make();
    CHECK(f.Install(pass) && f.Install(pass));
    auto context = Fixture::Context(Block0);
    pass.Run(P::kDraw, context, P::kDraw);
    CHECK(f.reserved.size() == 1 && f.frames == 1 && f.environments == 1 && f.originals == 0);
    CHECK(!f.reserved[0]->Ready() && !pass.CanCheckpoint());
    bool early = false;
    try { f.reserved[0]->Draws(); } catch (const std::logic_error&) { early = true; }
    CHECK(early);
    // The second draw of the same frame appends to the same material batch.
    context = Fixture::Context(Block2);
    pass.Run(P::kDraw, context, 0);
    CHECK(f.reserved.size() == 1 && f.frames == 1);
    auto forget = Fixture::Context(0);
    pass.Run(P::kForget, forget, P::kForget);
    CHECK(f.originals == 1 && f.reserved[0]->Ready());
    // Chain of two plus detached one, in source call order.
    auto expected = Expected(f, 2);
    const auto single = Expected(f, 1);
    expected.insert(expected.end(), single.begin(), single.end());
    CHECK(expected.size() == 12 && Same(f.reserved[0]->Draws(), expected));
    CHECK(f.reserved[0]->Draws()[0].src_frame == 7);
    auto counts = pass.Counts();
    CHECK(counts.calls == 2 && counts.replaced == 2 && counts.model_calls == 0 && counts.interior == 0);
    CHECK(counts.batches == 1 && counts.blocks == 3 && counts.draws == 12 && counts.forgets == 1);
    CHECK(!pass.CanCheckpoint());
    CHECK(f.reserved[0]->Finish(true) && f.reserved[0]->Complete() && !f.reserved[0]->Finish(true));
    CHECK(f.reserved[0]->Complete() == false); // duplicate delivery is a sticky failure
    // The sticky consumer failure surfaces before any new source work.
    bool faulted = false;
    try { context = Fixture::Context(Block0); pass.Run(P::kDraw, context, P::kDraw); }
    catch (const std::runtime_error&) { faulted = true; }
    CHECK(faulted && pass.Counts().faults == 1); // consumer failure surfaced before new work
}

void SecondFrameAndCheckpoint() {
    Fixture f; auto pass = f.Make(); CHECK(f.Install(pass));
    for (unsigned frame = 0; frame < 3; ++frame) {
        auto context = Fixture::Context(Block2);
        pass.Run(P::kDraw, context, P::kDraw);
        auto forget = Fixture::Context(0);
        pass.Run(P::kForget, forget, P::kForget);
        CHECK(f.reserved.size() == frame + 1 && f.reserved[frame]->Ready());
        CHECK(!pass.CanCheckpoint());
        CHECK(f.reserved[frame]->Finish(true));
        CHECK(pass.CanCheckpoint());
    }
    CHECK(pass.Counts().completed == 3 && pass.Counts().batches == 3 && f.frames == 3);
    // A forget without an open batch is an ordinary source operation.
    auto forget = Fixture::Context(0);
    pass.Run(P::kForget, forget, P::kForget);
    CHECK(f.originals == 4 && pass.Counts().forgets == 4 && pass.CanCheckpoint());
    // Zero distortion still produces the material pass with culled-free geometry.
    auto context = Fixture::Context(Block2, 0, 0);
    pass.Run(P::kDraw, context, P::kDraw);
    pass.Run(P::kForget, forget, P::kForget);
    CHECK(f.reserved.back()->Ready() && f.reserved.back()->Draws().size() == 4);
}

void ModelArm() {
    Fixture f(false); auto pass = f.Make(); CHECK(f.Install(pass));
    auto context = Fixture::Context(Block0);
    pass.Run(P::kDraw, context, P::kDraw);
    pass.Run(P::kDraw, context, P::kDraw + 0x48);
    auto forget = Fixture::Context(0);
    pass.Run(P::kForget, forget, P::kForget);
    const auto counts = pass.Counts();
    CHECK(f.originals == 3 && counts.calls == 2 && counts.model_calls == 2 && counts.interior == 1);
    CHECK(counts.replaced == 0 && counts.batches == 0 && f.reserved.empty() && f.frames == 0);
    CHECK(pass.CanCheckpoint());
}

void Refusals() {
    auto expect_fault = [](Fixture& f, P& pass, CpuContext context, uint32_t entry = P::kDraw) {
        bool faulted = false;
        try { pass.Run(P::kDraw, context, entry); } catch (const std::runtime_error&) { faulted = true; }
        CHECK(faulted && pass.Counts().faults == 1 && !pass.CanCheckpoint() && f.originals == 0);
    };
    { Fixture f; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_fault(f, pass, Fixture::Context(Block0), P::kDraw + 0x48);
      CHECK(pass.Counts().interior == 1 && f.reserved.empty()); }
    { Fixture f; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_fault(f, pass, Fixture::Context(Block0, 1, 1, P::kMaterial + 0x40)); CHECK(f.reserved.empty()); }
    // Pre-reservation conditions REFUSE the frame to the model (no fault, no
    // reservation, the generated body runs and is counted) until the source
    // forgets the frame; a provable next frame is owned again.
    auto expect_refusal = [](Fixture& f, P& pass, CpuContext context) {
        pass.Run(P::kDraw, context, P::kDraw);
        const auto counts = pass.Counts();
        CHECK(counts.refused == 1 && counts.model_calls == 1 && counts.replaced == 0 && counts.faults == 0);
        CHECK(f.originals == 1 && f.reserved.empty() && counts.batches == 0 && !pass.CanCheckpoint());
        // Every later draw of the refused frame stays with the model.
        auto again = Fixture::Context(Block2);
        pass.Run(P::kDraw, again, P::kDraw);
        CHECK(pass.Counts().refused == 1 && pass.Counts().model_calls == 2 && f.originals == 2 && f.reserved.empty());
        auto forget = Fixture::Context(0);
        pass.Run(P::kForget, forget, P::kForget);
        CHECK(f.originals == 3 && pass.CanCheckpoint() && pass.Counts().forgets == 1);
    };
    { Fixture f; Write<uint32_t>(P::kMaterial + P::kWriterFirst, 0x12345); auto pass = f.Make(); CHECK(f.Install(pass));
      expect_refusal(f, pass, Fixture::Context(Block0)); // the model already owns the writer
      Write<uint32_t>(P::kMaterial + P::kWriterFirst, 0);
      auto c = Fixture::Context(Block0); pass.Run(P::kDraw, c, P::kDraw);
      CHECK(f.reserved.size() == 1 && pass.Counts().replaced == 1 && pass.Counts().refused == 1); }
    { Fixture f; Write<uint32_t>(Shader + 0x6c, 0x2faec8); auto pass = f.Make(); CHECK(f.Install(pass));
      expect_fault(f, pass, Fixture::Context(Block0)); CHECK(f.reserved.empty()); } // a foreign object is integrity
    { Fixture f; f.refuse_frame = true; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_refusal(f, pass, Fixture::Context(Block0)); // the frame owner has no completed scene
      CHECK(f.frames == 1);
      f.refuse_frame = false;
      auto c = Fixture::Context(Block0); pass.Run(P::kDraw, c, P::kDraw);
      CHECK(f.reserved.size() == 1 && pass.Counts().replaced == 1 && f.frames == 2); }
    { Fixture f; f.refuse_reserve = true; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_refusal(f, pass, Fixture::Context(Block0)); // RecordOwned false = unselected for this call
      CHECK(f.reserved.empty() && pass.Counts().batches == 0);
      f.refuse_reserve = false;
      auto c = Fixture::Context(Block0); pass.Run(P::kDraw, c, P::kDraw);
      CHECK(f.reserved.size() == 1 && pass.Counts().replaced == 1 && pass.Counts().batches == 1); }
    { Fixture f; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_fault(f, pass, Fixture::Context(0x201000)); // unowned block chain, after the reservation
      CHECK(f.reserved.size() == 1 && !f.reserved[0]->Ready()); }
    { Fixture f; Write<uint32_t>(title::MaterialState::kDisplayWidth, 640); auto pass = f.Make(); CHECK(f.Install(pass));
      expect_refusal(f, pass, Fixture::Context(Block0)); } // frame/target extent disagreement
    { Fixture f; f.hooks.settings = nullptr; auto pass = f.Make(); CHECK(!f.Install(pass)); }
    // A live material whose shader was never observed by the settings owner
    // (missed constructor, restored state without the version) is refused.
    { Fixture f; f.unversioned_settings = true; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_refusal(f, pass, Fixture::Context(Block0)); CHECK(f.frames == 1); }
    { Fixture f; f.hooks.reserve = nullptr; auto pass = f.Make(); CHECK(!f.Install(pass)); }
    { Fixture f; auto pass = f.Make();
      bool rolled = false; unsigned installs = 0;
      CHECK(!pass.Install([&](uint32_t) { return P::Body([](CpuContext&, uint32_t) {}); },
                          [&](uint32_t, P::Body) { return ++installs != 2 || (rolled = true, false); }));
      CHECK(rolled && installs == 4); }
    // A consumer that finishes before source closure fails the batch; the
    // producer faults at its next source operation instead of replaying.
    { Fixture f; auto pass = f.Make(); CHECK(f.Install(pass));
      auto c = Fixture::Context(Block0); pass.Run(P::kDraw, c, P::kDraw);
      CHECK(!f.reserved[0]->Finish(true) && f.reserved[0]->Failed());
      expect_fault(f, pass, Fixture::Context(Block2)); }
    // A faulted pass does not replay the model; reset after the fence recovers.
    { Fixture f; auto pass = f.Make(); CHECK(f.Install(pass));
      expect_fault(f, pass, Fixture::Context(Block0), P::kDraw + 0x48);
      bool again = false;
      try { auto c = Fixture::Context(Block0); pass.Run(P::kDraw, c, P::kDraw); }
      catch (const std::runtime_error&) { again = true; }
      CHECK(again && f.originals == 0 && pass.Counts().faults == 1);
      pass.ResetAfterQuiescence();
      CHECK(pass.CanCheckpoint());
      auto c = Fixture::Context(Block0); pass.Run(P::kDraw, c, P::kDraw);
      CHECK(f.reserved.size() == 1); }
}
}  // namespace

int main() {
    try {
        OrderedFrames(); SecondFrameAndCheckpoint(); ModelArm(); Refusals();
        std::printf("DistortionPass: %u checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
