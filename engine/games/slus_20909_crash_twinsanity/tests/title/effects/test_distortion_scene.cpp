#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_fixture.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion scene line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
}
int main() {
    try {
        using E = title::EffectsScene;
        EffectsFixture scene;
        DistortionFixture input(scene.frame.height, scene.frame.source_frame);
        CHECK(input.draws.size() == 4);
        const std::vector<uint8_t> mirror(scene.memory.Bytes(), scene.memory.Bytes() + ps2r::gs::VramImage::kBytes);
        scene.pipeline.SetGfxEpoch(789);
        const auto before = scene.pipeline.DrawSeq();
        const int draws = scene.backend.draws, readbacks = scene.backend.readbacks;
        const auto run = [&] { return scene.effects.Distortion(scene.frame, input.environment, input.draws); };
        CHECK(run() == E::Result::Ok);
        CHECK(scene.backend.draws == draws + 5 && scene.backend.readbacks == readbacks);
        CHECK(scene.effects.DistortionCounts().copies == 1 && scene.effects.DistortionCounts().batches == 4);
        CHECK(scene.effects.DistortionCounts().triangles == 72 && scene.effects.DistortionCounts().allocations == 3);
        CHECK(scene.pipeline.DrawSeq() == before + 4 && scene.pipeline.GfxEpoch() == 789);
        CHECK(scene.effects.Counts().distortion_passes == 1);
        CHECK(scene.backend.last_params.scale == 2 && scene.backend.last_params.color.id &&
              scene.backend.last_params.depth.id && scene.backend.last_params.rt_copy.id);
        CHECK(std::memcmp(mirror.data(), scene.memory.Bytes(), mirror.size()) == 0);
        CHECK(run() == E::Result::Ok && scene.effects.DistortionCounts().allocations == 3);
        CHECK(scene.effects.DistortionCounts().copies == 2 && scene.effects.DistortionCounts().triangles == 144);
        // Source values are no longer needed after the owned geometry was
        // delivered. Only the exact shared surface leases are consulted.
        input.look = {}; input.blocks = {}; input.view = {};
        CHECK(run() == E::Result::Ok);
        const auto accepted_draws = scene.backend.draws;
        input.draws[0].tex.tw = 9;
        CHECK(run() == E::Result::InvalidInput && scene.backend.draws == accepted_draws);
        input.draws[0].tex.tw = 8;
        input.draws[0].src_frame++;
        CHECK(run() == E::Result::InvalidInput && scene.backend.draws == accepted_draws);
        input.draws[0].src_frame--;
        // A wholly culled source batch still executes one real copy; it
        // cannot be counted as positive geometry work.
        const auto triangles = scene.effects.DistortionCounts().triangles;
        CHECK(scene.effects.Distortion(scene.frame, input.environment, {}) == E::Result::Ok);
        CHECK(scene.effects.DistortionCounts().copies == 4 && scene.effects.DistortionCounts().triangles == triangles);
        scene.backend.fail_after_draws = scene.backend.draws + 2;
        CHECK(run() == E::Result::DrawFailure);
        const auto failed_draws = scene.backend.draws;
        CHECK(run() == E::Result::Faulted && scene.backend.draws == failed_draws);
        CHECK(scene.effects.DistortionCounts().failures == 1);
        scene.effects.ResetAfterQuiescence();
        scene.backend.fail_after_draws = -1;
        CHECK(run() == E::Result::Ok);
        CHECK(scene.effects.DistortionCounts().allocations == 6);
        CHECK(std::memcmp(mirror.data(), scene.memory.Bytes(), mirror.size()) == 0);
        std::printf("DistortionScene: %u checks; owned copy/geometry on shared scene, failure without replay\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
