#pragma once

#include <memory>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_pass.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_fixture.h"

// Flat guest RAM with the retail distortion material object, render table,
// view matrix, frame globals and three owned particle blocks.
class DistortionPassFixture {
public:
    using P = title::DistortionPass;
    static constexpr uint32_t Shader = 0x380000, Table = 0x390000, Matrix = 0x3a0000, Target = 0x3b0000;
    static constexpr uint32_t Block0 = 0x200000, Block1 = 0x200200, Block2 = 0x200400;
    DistortionFixture source{448, 7};
    title::DistortionBlocks blocks;
    std::vector<std::shared_ptr<const P::Batch>> reserved;
    unsigned originals = 0, frames = 0, environments = 0;
    bool refuse_reserve = false, refuse_frame = false, unversioned_settings = false;
    P::Hooks hooks;
    explicit DistortionPassFixture(bool enabled = true);
    P Make();
    bool Install(P& pass);
    static bool Reader(uint32_t at, void* out, size_t size);
    template<class T> static void Write(uint32_t at, const T& value) { std::memcpy(Ram() + at, &value, sizeof(T)); }
    static CpuContext Context(uint32_t block, float dx = 3.0f / 0x1p-14f, float dy = .75f / -0x1p-12f,
                              uint32_t material = P::kMaterial);
private:
    static uint8_t* Ram();
};
