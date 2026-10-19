#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "tests/lib/fake_backend.h"

namespace ps2r::test {
class StripSceneFixture {
public:
    struct Backend : FakeBackend {
        gs::DrawIr last;
        gs::TextureSource texture;
        render::DrawResult Draw(const gs::DrawIr& draw,const gs::TextureSource& tex,
                                const render::DrawParams& params) override {
            last = draw; texture = tex;
            return FakeBackend::Draw(draw,tex,params);
        }
    };
    Backend backend;
    ::title::GraphicsSurfaces surfaces;
    gs::VramImage memory;
    gs::VramLayout layout;
    gs::ScalePolicy scale;
    gs::RenderTargetCache cache{surfaces,memory,scale};
    gs::TargetRefresher refresher{surfaces,memory,cache};
    gs::TextureResolver resolver{surfaces,memory,cache,refresher,layout};
    gs::ScanoutComposer scanout{surfaces,cache};
    gs::Presenter presenter{scanout};
    gs::SequenceRecipe sequence{gs::SequenceRecipe::Constants{}};
    gs::DrawPipeline pipeline{gs::DrawPipeline::Deps{
        &surfaces,&memory,&cache,&refresher,&resolver,&scanout,&presenter,&layout,nullptr,&sequence}};
    ::title::GraphicsResources resources{surfaces};
    ::title::GraphicsRenderer renderer{surfaces,resources};
    ::title::GraphicsScene scene{surfaces,resources,renderer,pipeline};
    ::title::GraphicsScene::Frame frame;
    explicit StripSceneFixture(const ::title::GraphicsScene::Frame& source,
                               render::Backend* external = nullptr,uint8_t factor = 1)
        : surfaces(external ? *external : backend),scale(gs::ScaleSettings{factor},&layout),frame(source) {
        pipeline.OnSetWindow(int(frame.width),int(frame.height),int(frame.width),int(frame.height));
        pipeline.OnSetFrontBuffer(frame.color_buffer,frame.width,0);
    }
};
} // namespace ps2r::test
