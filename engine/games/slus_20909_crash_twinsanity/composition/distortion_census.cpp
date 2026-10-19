#include "games/slus_20909_crash_twinsanity/composition/distortion_application.h"

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_report.h"

namespace ps2r::app {
void DistortionApplication::Record(verify::RunMeta& meta) const {
    ::title::EffectsReport::Distortion(meta, Selected(), Counts());
}
}  // namespace ps2r::app
