#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_material.h"

class DistortionMaterialOracle {
public:
    static constexpr uint32_t kShader = 0x10000, kTarget = 0x11000, kCopy = 0x180000;
    title::MaterialState::Frame frame;
    std::array<uint32_t, 8> target{};
    void Reset(title::DistortionMaterial::Settings, uint32_t height, unsigned variant);
    // Actual complete generated setup/copy bodies. Optional later writes match
    // the source constructor's ordering and must not change captured settings.
    std::vector<ps2r::gs::DrawIr> Original(bool late_fields = false);
    title::MaterialState::Read Reader(uint32_t deny = 0) const;
    static uint64_t Calls();
};
