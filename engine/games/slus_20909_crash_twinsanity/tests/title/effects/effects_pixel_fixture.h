#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"

class EffectsPixelFixture {
public:
    title::GraphicsSurfaces surfaces;
    ps2r::gs::VramImage memory;
    ps2r::gs::VramLayout layout;
    ps2r::gs::ScalePolicy scale;
    ps2r::gs::RenderTargetCache cache;
    ps2r::gs::TargetRefresher refresher;
    ps2r::gs::TextureResolver resolver;
    ps2r::gs::ScanoutComposer scanout;
    ps2r::gs::Presenter presenter;
    ps2r::gs::SequenceRecipe sequence{ps2r::gs::SequenceRecipe::Constants{}};
    ps2r::gs::DrawPipeline pipeline;
    title::GraphicsResources resources;
    title::GraphicsRenderer renderer;
    title::GraphicsScene scene;
    title::EffectsScene effects;
    title::EffectsScene::Frame frame{0x70000, 0x150000, 512, 448, 7, 31};

    EffectsPixelFixture(ps2r::render::Backend&, uint8_t scale, uint32_t height);
    ~EffectsPixelFixture();
    void Initial(const ps2r::gs::DrawIr& full_rect, const std::vector<uint8_t>& rgba, float depth);
    std::vector<uint8_t> Pixels();
};
