#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_geometry.h"

// Complete original NTSC program + unchanged NEW VIF/interpreter/vertex output.
// The original arm consumes authored bytes independently of the title decoder.
class ClothOracle {
public:
    explicit ClothOracle(const std::vector<uint8_t>& original_elf);
    std::vector<ps2r::gs::DrawIr> Run(const std::vector<uint8_t>& asset,
        const title::ClothGeometry::Waves&, const std::vector<title::MeshGeometry::View>&,
        const ps2r::gs::DrawIr& state) const;
private:
    struct Program { uint32_t address; std::vector<uint8_t> bytes; };
    std::vector<Program> common_;
    std::array<std::vector<uint8_t>, 2> cloth_;
};
