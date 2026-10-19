#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_fixture.h"

#include <cstdio>
#include <cstring>

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
using Result = title::EffectsScene::Result;
void SharedScene() {
    EffectsFixture world;
    world.pipeline.SetGfxEpoch(9001);
    CHECK(world.pipeline.DrawGeneric(world.OrdinaryDraw()).drew);
    const auto ordinary = world.backend.last_params;
    auto* target = world.cache.Find(world.frame.color_buffer);
    const auto depth_sequence = target->coherence.last_z_seq;
    const std::vector<uint8_t> original(world.memory.Bytes(),
                                        world.memory.Bytes() + ps2r::gs::VramImage::kBytes);
    const auto volume = ShadowFixture::Volume(256, 224, 8, 0x123456);
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.pipeline.DrawSeq() == 2 && target->coherence.main_author_seq == 2);
    CHECK(world.pipeline.GfxEpoch() == 9001 && target->coherence.last_draw_gfx_epoch == 9001);
    CHECK(target->coherence.last_z_seq == depth_sequence); // composite protects scene depth
    CHECK(world.backend.last_params.color.id == ordinary.color.id);
    CHECK(world.backend.last_params.depth.id == ordinary.depth.id);
    CHECK(world.backend.last_params.scale == 2 && !world.backend.last_params.do_frame_clear);
    CHECK(target->coherence.cd_copy.dirty_x1 == 512 && target->coherence.cd_copy.dirty_y1 == 448);
    CHECK(world.cache.MostRecentDrawnFbp() == world.frame.color_buffer);
    CHECK(world.backend.creates == 9 && world.backend.readbacks == 0 && world.backend.uploads == 0);
    CHECK(std::memcmp(original.data(), world.memory.Bytes(), original.size()) == 0);
    CHECK(world.effects.Counts().shadow_passes == 1 && world.effects.ShadowCounts().strips == 1);
    CHECK(world.pipeline.DrawGeneric(world.OrdinaryDraw()).drew);
    CHECK(!world.backend.last_params.do_frame_clear && world.pipeline.DrawSeq() == 3);
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.backend.creates == 9); // intermediates reused, exact scene leases reacquired
    world.effects.ResetAfterQuiescence();
    world.scene.CollectAfterQuiescence();
    CHECK(world.resources.Counts().live_bindings == 0);
    CHECK(world.backend.Has(ordinary.color) && world.backend.Has(ordinary.depth));
    CHECK(world.backend.destroys == 6);
}
void ClearBoundary() {
    EffectsFixture world;
    auto& target = world.cache.GetOrCreate(world.frame.color_buffer, 512, 448, 0, 31);
    const auto volume = ShadowFixture::Volume(256, 224, 8, 0x123456);
    const int allocations_cleared = world.backend.clears;
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.backend.clears == allocations_cleared + 1);
    CHECK(!target.WantsFrameClear(world.presenter.EeFrameCount()));
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.backend.clears == allocations_cleared + 1);
    world.presenter.MarkEeFrameEnd();
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.backend.clears == allocations_cleared + 2);
}
// A pass with no accepted mesh publishes NOTHING (the s3 play bug: the empty
// owned pass still drew its clear/copies/composite every frame). No scene
// lease is acquired, no draw, no clear, no publication, no scene clear consumed
// -- even when the scene would refuse -- and the pass is counted as empty.
void EmptyPass() {
    EffectsFixture world;
    auto& target = world.cache.GetOrCreate(world.frame.color_buffer, 512, 448, 0, 31);
    const int clears = world.backend.clears, creates = world.backend.creates;
    CHECK(world.effects.Shadows(world.frame, {}) == Result::Ok);
    CHECK(world.backend.draws == 0 && world.backend.creates == creates && world.backend.clears == clears);
    CHECK(world.pipeline.DrawSeq() == 0 && target.WantsFrameClear(world.presenter.EeFrameCount()));
    CHECK(world.resources.Counts().live_bindings == 0);
    CHECK(world.effects.Counts().empty_shadow_passes == 1 && world.effects.Counts().shadow_passes == 0 &&
          world.effects.Counts().rejected == 0);
    world.cache.AliasPages().MarkAliased(world.frame.color_buffer, 8192);
    CHECK(world.effects.Shadows(world.frame, {}) == Result::Ok);
    CHECK(world.effects.Counts().empty_shadow_passes == 2 && world.effects.Counts().rejected == 0 &&
          world.backend.draws == 0);
    // A non-empty pass after empty ones is the first published one.
    world.cache.AliasPages().ClearAliased(world.frame.color_buffer, 8192);
    const auto volume = ShadowFixture::Volume(256, 224, 8, 0x123456);
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.effects.Counts().shadow_passes == 1 && world.pipeline.DrawSeq() == 1 && world.backend.draws > 0);
}
void Refusals() {
    for (unsigned variant = 0; variant < 5; ++variant) {
        EffectsFixture world;
        world.cache.GetOrCreate(world.frame.color_buffer, 512, 448, 0, 31);
        const int allocation_clears = world.backend.clears;
        // Aliased / CPU-dirty pages whose author is not this target (the cold
        // mirror here) are a foreign dependency; a ledger entry on ANOTHER
        // buffer is never settled for this frame and stays owed.
        if (variant == 0) world.cache.AliasPages().MarkAliased(world.frame.color_buffer, 8192);
        if (variant == 1) world.cache.AliasPages().MarkCpuDirty(world.frame.color_buffer, 8192);
        if (variant == 2) {
            world.memory.NoteCpuWrite(ps2r::gs::VramImage::Span{world.frame.color_buffer, 8192});
            world.cache.AliasPages().MarkAliased(world.frame.color_buffer, 8192);
        }
        if (variant == 3) world.sequence.Prime(0x230000);
        if (variant == 4) world.frame.width += 64;
        const auto volume = ShadowFixture::Volume(256, 224, 8, 0x123456);
        CHECK(world.effects.Shadows(world.frame, {volume}) == Result::SceneUnavailable);
        CHECK(world.backend.draws == 0 && world.backend.clears == allocation_clears);
        CHECK(world.backend.readbacks == 0 && world.backend.uploads == 0);
        CHECK(world.effects.Counts().rejected == 1 && world.effects.Counts().shadow_passes == 0);
    }
    {
        // A deferred writeback still owing THIS buffer's rows is SETTLED before
        // the owned pass (GraphicsScene::SettleBeforeDraw -- the retained arm's
        // SameBufferPreDrawFlush rule): the entry is deposited and gone, the
        // pass runs; an entry on another buffer stays deferred. The target's
        // OWN writeback (pages it authored) is not a dependency either; the
        // same pages deposited by another target are.
        EffectsFixture world;
        const auto volume = ShadowFixture::Volume(256, 224, 8, 0x123456);
        world.cache.GetOrCreate(world.frame.color_buffer, 512, 448, 0, 31);
        world.cache.Ledger().Record(world.frame.color_buffer, 8192, 1);
        world.cache.Ledger().Record(0x3f0000, 8192, 1);
        const auto settles = world.scene.PreDrawSettles();
        CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
        CHECK(world.backend.uploads == 0);
        CHECK(world.scene.PreDrawSettles() == settles + 1 &&
              !world.cache.Ledger().Has(world.frame.color_buffer) && world.cache.Ledger().Has(0x3f0000) &&
              world.cache.Ledger().Flushes(ps2r::gs::PendingRtSyncLedger::Reason::PreDraw) == 1);
        CHECK(world.effects.Counts().rejected == 0 && world.effects.Counts().shadow_passes == 1);
        world.cache.Ledger().Remove(0x3f0000);
        world.cache.AliasPages().MarkCpuDirty(world.frame.color_buffer, 8192);
        world.cache.AliasPages().MarkAliased(world.frame.color_buffer, 8192);
        world.memory.NoteGpuAuthor(ps2r::gs::VramImage::Span{world.frame.color_buffer, 8192}, world.frame.color_buffer);
        CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
        CHECK(world.effects.Counts().rejected == 0 && world.effects.Counts().shadow_passes == 2);
        world.memory.NoteGpuAuthor(ps2r::gs::VramImage::Span{world.frame.color_buffer, 8192}, 0x3f0000);
        CHECK(world.effects.Shadows(world.frame, {volume}) == Result::SceneUnavailable);
        CHECK(world.effects.Counts().rejected == 1 && world.effects.Counts().shadow_passes == 2);
    }
    EffectsFixture world;
    auto volume = ShadowFixture::Volume(256, 224, 8, 0);
    volume.verts[0].r = 42;
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::InvalidInput);
    CHECK(world.backend.creates == 0 && world.backend.draws == 0 && world.backend.clears == 0);
}
void FailureAndReset() {
    EffectsFixture world;
    const auto volume = ShadowFixture::Volume(256, 224, 8, 0x123456);
    world.backend.fail_after_draws = 2;
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::DrawFailure);
    CHECK(world.effects.Counts().failures == 1 && world.pipeline.DrawSeq() == 0);
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Faulted);
    CHECK(world.backend.draws == 2); // no partial replay after mutation
    world.effects.ResetAfterQuiescence();
    world.scene.CollectAfterQuiescence();
    CHECK(world.resources.Counts().live_bindings == 0);
    world.backend.fail_after_draws = -1;
    CHECK(world.effects.Shadows(world.frame, {volume}) == Result::Ok);
    CHECK(world.effects.Counts().shadow_passes == 1 && world.pipeline.DrawSeq() == 1);
}
void WaveScene() {
    EffectsFixture world;
    title::WaveGeometry::Grid grid;
    CHECK(title::WaveGeometry::MakeGrid(512, 448, grid) == title::WaveGeometry::Result::Ok);
    CHECK(world.effects.Wave(world.frame, grid, {}) == Result::Ok);
    CHECK(world.effects.Counts().wave_passes == 1 && world.effects.WaveCounts().strips == 64);
    CHECK(world.pipeline.DrawSeq() == 64);
    CHECK(world.backend.creates == 5 && world.backend.readbacks == 0 && world.backend.uploads == 0);
    const auto* target = world.cache.Find(world.frame.color_buffer);
    CHECK(target && target->coherence.main_author_seq == 64 && target->coherence.last_z_seq == 0);
    CHECK(world.effects.Wave(world.frame, grid, {}) == Result::Ok);
    CHECK(world.backend.creates == 5 && world.pipeline.DrawSeq() == 128);
    world.effects.ResetAfterQuiescence();
    world.scene.CollectAfterQuiescence();
    CHECK(world.resources.Counts().live_bindings == 0 && world.backend.destroys == 2);
    grid.strips.clear();
    const auto before = world.backend.draws;
    CHECK(world.effects.Wave(world.frame, grid, {}) == Result::InvalidInput);
    CHECK(world.backend.draws == before);
}
}  // namespace
int main() {
    SharedScene();
    ClearBoundary();
    EmptyPass();
    Refusals();
    FailureAndReset();
    WaveScene();
    std::printf("EffectsScene: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
