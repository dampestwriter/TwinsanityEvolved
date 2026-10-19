#pragma once

#include <string>

#include "engine/core/verify/run_meta.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_library.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_pass.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_library.h"

namespace title {

// Title-owned run.meta publication of the effects replaced-model census
// (shadow bucket 21, distortion bucket 23, screen effects bucket 4), the single publication site the
// reviewed counter catalog binds together with each increment site in
// ShadowPass / DistortionPass. Keys and `name=value,...` formats are the
// gate's parsed contract -- change them only together with the catalog. The
// root passes the counts it already composed (live plus retired owners).
class EffectsReport {
public:
    using Meta = ps2r::verify::RunMeta;
    // graphics_shadow_{begin,mesh,end}: the three source shadow boundaries.
    static void Shadow(Meta& meta, bool selected, const ShadowLibrary::Counters& counts);
    // graphics_distortion_draw: the DrawHexagonParticleBlock body (0x1b03f0).
    static void Distortion(Meta& meta, bool selected, const DistortionLibrary::Counters& counts);
    // graphics_screen_effects: the DrawScreenEffects boundary (0x1b92d8),
    // bucket 4; graphics_screen_scene: the owned deliveries. Unselected, the
    // scene/renderer counters are zero.
    static void Screen(Meta& meta, bool selected, const ScreenPass::Counters& counts,
                       const EffectsScene::Counters& scene, const ScreenRenderer::Counters& renderer,
                       const WaveRenderer::Counters& wave);

private:
    static std::string Row(const ShadowPass::Count& count);
};

}  // namespace title
