#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <cstdio>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"

namespace {
using namespace ps2r;
using Scene = title::GraphicsScene;
using State = title::SceneState;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)

class TestGraphicsCheckpointGl {
public:
    title::GraphicsSurfaces surfaces;
    gs::VramImage vram;
    gs::VramLayout layout;
    gs::ScalePolicy scale{gs::ScaleSettings{2},&layout};
    gs::RenderTargetCache cache{surfaces,vram,scale};
    gs::TargetRefresher refresher{surfaces,vram,cache};
    gs::TextureResolver resolver{surfaces,vram,cache,refresher,layout};
    gs::ScanoutComposer scanout{surfaces,cache};
    gs::Presenter presenter{scanout};
    gs::SequenceRecipe sequence{gs::SequenceRecipe::Constants{}};
    gs::DrawPipeline pipeline{gs::DrawPipeline::Deps{
        &surfaces,&vram,&cache,&refresher,&resolver,&scanout,&presenter,&layout,nullptr,&sequence}};
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces,resources};
    Scene scene{surfaces,resources,renderer,pipeline};
    Scene::Frame frame{0x70000,0x150000,64,32,3,19};
    explicit TestGraphicsCheckpointGl(render::Backend& backend) : surfaces(backend) {
        pipeline.OnSetWindow(64,32,128,64);
        pipeline.OnSetFrontBuffer(frame.color_buffer,64,0);
    }
    gs::DrawIr Triangle(unsigned pass) const {
        gs::DrawIr draw;
        draw.src_frame = frame.source_frame; draw.prim_type = 3;
        draw.raster.frame = {frame.color_buffer,frame.width,0,0};
        draw.raster.zbuf = {frame.depth_buffer,0,false};
        draw.raster.test.zte = true; draw.raster.test.ztst = 2;
        draw.raster.colclamp = true;
        draw.raster.scissor = {0,0,63,31};
        draw.verts.resize(3); draw.vertex_count = 3;
        draw.verts[0].x = 3; draw.verts[0].y = 2;
        draw.verts[1].x = 61; draw.verts[1].y = 5;
        draw.verts[2].x = 5; draw.verts[2].y = 29;
        for (auto& v : draw.verts) {
            v.z = pass ? 0x60000000 : 0x40000000; v.q = 1;
            v.r = pass ? 195 : 47; v.g = 23; v.b = pass ? 18 : 109; v.a = 64;
        }
        return draw;
    }
    void Typed(const gs::DrawIr& draw) {
        title::Graphics queue;
        title::GraphicsTargets targets(queue);
        Scene::Binding binding;
        CHECK(scene.Bind(queue,targets,frame,binding) == Scene::Result::Ok);
        CHECK(queue.BeginFrame(frame.source_frame) == title::Graphics::Result::Ok);
        CHECK(targets.Record(queue.End(24),draw) == title::GraphicsTargets::Result::Ok);
        title::Graphics::Hooks hooks;
        hooks.draw = [&](const auto& ir,const auto& leases) {
            return scene.Draw(ir,leases) == Scene::Result::Ok;
        };
        hooks.drain = [] { return true; };
        CHECK(queue.Submit(hooks) == title::Graphics::Result::Ok);
        targets.Reset();
        Scene::Release(queue,binding);
        scene.CollectAfterQuiescence();
    }
    State::Snapshot Snapshot() {
        State::Snapshot snapshot;
        std::string error;
        CHECK(scene.CaptureState(snapshot,&error));
        if (!error.empty()) std::fprintf(stderr,"%s\n",error.c_str());
        return snapshot;
    }
};

void Continuation(render::GlDevice& backend, unsigned material) {
    TestGraphicsCheckpointGl f(backend);
    CHECK(f.pipeline.DrawGeneric(f.Triangle(0)).drew);
    auto next = f.Triangle(1);
    next.alpha_blend = true;
    // OLD alpha/FBA/subtractive cases: destination bytes and depth must survive
    // save/restore before the same typed+retained continuation.
    next.raster.alpha = material & 1 ? gs::DrawIr::Alpha{2,0,0,1,0} :
                                      gs::DrawIr::Alpha{0,1,0,1,0};
    next.raster.fba = material & 2;
    next.raster.frame.fbmsk = material & 4 ? 0xff000000 : 0;
    auto* target = f.cache.Find(f.frame.color_buffer);
    CHECK(target && target->identity.scale == 2);
    // Force a shader blend to author a valid lazy destination copy in the
    // seed, exercising its actual contents and dirty rectangle at restore.
    auto seed_blend = next;
    seed_blend.raster.alpha = {0,1,2,1,63};
    seed_blend.raster.fba = true;
    CHECK(f.pipeline.DrawGeneric(seed_blend).drew);
    const auto baseline = f.Snapshot();
    CHECK(baseline.targets.size() == 1 && baseline.targets[0].history.cd_copy.valid &&
          !baseline.targets[0].copy.empty());
    f.Typed(next);
    CHECK(f.pipeline.DrawGeneric(next).drew);
    const auto expected = f.Snapshot();
    const auto draws = f.pipeline.DrawSeq();
    std::vector<uint8_t> bytes;
    CHECK(State::Encode(baseline,bytes));
    State::Snapshot decoded;
    CHECK(State::Decode(bytes,decoded));
    CHECK(f.scene.StageState(decoded));
    CHECK(f.scene.PublishState());
    CHECK(f.pipeline.DrawSeq() == draws);
    f.Typed(next);
    CHECK(f.pipeline.DrawGeneric(next).drew);
    const auto actual = f.Snapshot();
    std::vector<uint8_t> reference, restored;
    CHECK(State::Encode(expected,reference) && State::Encode(actual,restored));
    CHECK(reference == restored); // actual GL raster, all physical attachments/history
    CHECK(f.pipeline.DrawSeq() == draws + 2);
    CHECK(glGetError() == GL_NO_ERROR);
}
}

int main() {
    platform::Window window;
    platform::GlContext context;
    if (!window.Create(true,128,64,"Graphics checkpoint") || !context.Create(window)) return 77;
    render::GlDevice backend;
    CHECK(backend.Init([&] { window.Swap(); }));
    backend.SetWindowSize(128,64);
    for (unsigned material = 0; material < 8; ++material) Continuation(backend,material);
    backend.Shutdown();
    std::printf("GraphicsCheckpointGL: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
