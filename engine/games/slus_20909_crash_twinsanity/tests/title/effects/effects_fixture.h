#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_backend.h"

class EffectsFixture {
public:
    EffectsBackend backend;
    title::GraphicsSurfaces surfaces{backend};
    ps2r::gs::VramImage memory;
    ps2r::gs::VramLayout layout;
    ps2r::gs::ScalePolicy scale{ps2r::gs::ScaleSettings{2}, &layout};
    ps2r::gs::RenderTargetCache cache{surfaces, memory, scale};
    ps2r::gs::TargetRefresher refresher{surfaces, memory, cache};
    ps2r::gs::TextureResolver resolver{surfaces, memory, cache, refresher, layout};
    ps2r::gs::ScanoutComposer scanout{surfaces, cache};
    ps2r::gs::Presenter presenter{scanout};
    ps2r::gs::SequenceRecipe sequence{ps2r::gs::SequenceRecipe::Constants{}};
    ps2r::gs::DrawPipeline pipeline{ps2r::gs::DrawPipeline::Deps{
        &surfaces, &memory, &cache, &refresher, &resolver, &scanout, &presenter, &layout, nullptr,
        &sequence}};
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces, resources};
    title::GraphicsScene scene{surfaces, resources, renderer, pipeline};
    title::EffectsScene effects{scene, resources, pipeline};
    title::EffectsScene::Frame frame{0x70000, 0x150000, 512, 448, 7, 31};
    EffectsFixture();
    ~EffectsFixture();
    ps2r::gs::DrawIr OrdinaryDraw() const;
};
