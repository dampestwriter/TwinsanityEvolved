#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_renderer.h"

class ShadowFixture {
public:
    ShadowFixture(ps2r::render::Backend&, uint32_t width, uint32_t height, uint8_t scale = 1);
    title::GraphicsResources resources;
    title::Graphics registry;
    title::ShadowRenderer::Scene scene;
    std::array<ps2r::render::SurfaceId, 3> images;
    uint64_t publications = 0;
    bool publication_ok = true;
    static ps2r::gs::DrawIr Volume(uint32_t width, uint32_t height, uint8_t colour, float depth);
};
