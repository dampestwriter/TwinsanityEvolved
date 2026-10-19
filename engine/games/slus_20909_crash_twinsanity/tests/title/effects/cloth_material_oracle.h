#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_material.h"

class ClothMaterialOracle {
public:
    static constexpr uint32_t kMaterial = 0x10000, kShader = 0x11000, kTexture = 0x12000;
    static constexpr uint32_t kTarget = 0x13000, kWaves = 0x14000;
    explicit ClothMaterialOracle(const std::vector<uint8_t>& elf);
    void Reset(title::ClothGeometry::Kind kind, uint64_t settings, int16_t k, int16_t l,
               unsigned levels, unsigned variant);
    ps2r::gs::DrawIr Original();
    title::MaterialState::Read Reader(uint32_t deny = 0) const;
    title::MeshMaterial::Texture texture;
    title::MaterialState::Environment environment;
    uint64_t Calls() const;
private:
    std::vector<uint8_t> elf_;
};
