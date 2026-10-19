#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/wave_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

namespace title {

// Runs a complete special pass in the existing ordered Graphics output slot.
// It shares the actual scene allocation and publication protocol; private pass
// images never enter the retained address map or a texture resolver.
class EffectsScene {
public:
    using Frame = GraphicsScene::Frame;
    enum class Result { Ok, InvalidInput, SceneUnavailable, ResourceFailure, DrawFailure, Faulted };
    struct Counters {
        uint64_t shadow_passes = 0, wave_passes = 0, distortion_passes = 0, rejected = 0, failures = 0;
        // Shadow passes with no volume: nothing is acquired, drawn or published.
        uint64_t empty_shadow_passes = 0;
        uint64_t screen_passes = 0;
    };
    // The complete bucket-4 DrawScreenEffects frame in source order: blur,
    // filter (ScreenRenderer), then the wave grid (WaveRenderer) when switched
    // on. The grid is the source's MakeWaveGrid of the pass height.
    struct ScreenFrame {
        ScreenRenderer::Pass screen;
        bool wave = false;
        WaveGeometry::Waves waves;
    };

    EffectsScene(GraphicsScene&, GraphicsResources&, ps2r::gs::DrawPipeline&);
    ~EffectsScene();
    // Backend owner only, after preceding Graphics work has drained. Values
    // were captured at the source producer; no guest memory is read here.
    Result Shadows(const Frame&, const std::vector<ps2r::gs::DrawIr>& volumes);
    Result Wave(const Frame&, const WaveGeometry::Grid&, const WaveGeometry::Waves&);
    Result Distortion(const Frame&, const MaterialState::Environment&,
                      const std::vector<ps2r::gs::DrawIr>&);
    Result Screen(const Frame&, const ScreenFrame&);
    // Requires the same accepted fence as GraphicsScene's resource collection.
    void ResetAfterQuiescence();
    Counters Counts() const;
    ShadowRenderer::Counters ShadowCounts() const;
    WaveRenderer::Counters WaveCounts() const;
    DistortionRenderer::Counters DistortionCounts() const;
    ScreenRenderer::Counters ScreenCounts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
