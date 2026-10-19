#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.h"

// Test-only original asset -> unchanged NEW VIF/interpreter/vertex assembly.
// No replacement decoder or replacement geometry is used by this arm.
class ShadowAssetOracle {
public:
    explicit ShadowAssetOracle(const std::vector<uint8_t>& verified_elf);
    // eye: source PlacedEyeSize instances (11 quadwords, the eye quadword filled
    // with poison) as every default shadow mesh has (its type-0B shader answers
    // NeedsEye); otherwise PlacedClippedSize 10.
    std::vector<ps2r::gs::DrawIr> Run(const std::vector<uint8_t>& asset,
                                    const std::vector<title::ShadowGeometry::View>&,
                                    bool eye = false) const;
private:
    struct Program { uint32_t address; std::vector<uint8_t> bytes; };
    std::vector<Program> programs_;
};
