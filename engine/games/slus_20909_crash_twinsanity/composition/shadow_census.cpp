#include "games/slus_20909_crash_twinsanity/composition/shadow_application.h"

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_report.h"

namespace ps2r::app {
void ShadowApplication::Record(verify::RunMeta& meta) const {
    ::title::EffectsReport::Shadow(meta, Selected(), Counts());
    // The consumer side of the owned passes: published = passes drawn and
    // composited into the scene; empty = delivered passes with no accepted mesh,
    // for which nothing was drawn (the s3 streak pin: an empty pass is invisible).
    const auto scene = SceneCounts();
    meta.Set("graphics_shadow_scene", "published=" + std::to_string(scene.shadow_passes) +
        ",empty=" + std::to_string(scene.empty_shadow_passes));
    Refusals().Record(meta, {::title::GraphicsRefusals::Family::Shadows});
}
}  // namespace ps2r::app
