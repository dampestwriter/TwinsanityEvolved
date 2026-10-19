#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "tests/lib/fake_backend.h"

namespace {
using namespace ps2r;
using Scene = title::GraphicsScene;
using Intent = title::FrameIntent;
using Display = title::GraphicsDisplay;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)
class TestGraphicsDisplay {
public:
    title::GraphicsSurfaces surfaces;
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
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces,resources};
    Scene scene{surfaces,resources,renderer,pipeline};
    title::GraphicsDisplay display;
    Scene::Frame frame{0x70000,0x150000,512,448,7,31};
    Intent::Output intent;
    explicit TestGraphicsDisplay(render::Backend& backend,uint8_t factor)
        : surfaces(backend),scale(gs::ScaleSettings{factor},&layout),
          display(scene,resources,renderer,pipeline,factor) {
        pipeline.OnSetWindow(512,448,512*factor,448*factor);
        CHECK(Intent::Build(frame.width,frame.height,{},intent) == Intent::Result::Ok);
    }
    void Source() {
        // Fixture authors an actual shared host attachment; production uses the
        // selected/retained scene producers. Alpha and depth are intentionally
        // different from the display-copy vertex and must not be overwritten.
        auto& t = cache.GetOrCreate(frame.color_buffer,512,448,0,0);
        surfaces.Clear(t.views.color,t.views.depth,0xb687472fu,0.375f);
        t.coherence.valid_rect.Union(0,0,512,448);
        t.coherence.has_content = true;
        t.identity.render_fbw = 512;
        t.identity.has_32bit_content = t.identity.was_32bit = true;
    }
};

void LifetimeAndCadence() {
    test::FakeBackend backend;
    TestGraphicsDisplay f(backend,2);
    CHECK(!f.display.Active());
    CHECK(f.display.Resolve(f.intent,f.frame) == Display::Resolution::Refused); // no parallel blank source
    CHECK(backend.creates == 0 && backend.draws == 0);
    f.Source();
    const auto* source = f.cache.Find(f.frame.color_buffer);
    const auto color = source->views.color,depth = source->views.depth;
    const auto original_color = backend.At(color).rgba;
    const auto original_depth = backend.At(depth).depth;
    const int clears = backend.clears, reads = backend.readbacks;
    CHECK(f.display.Resolve(f.intent,f.frame) == Display::Resolution::Ok);
    CHECK(f.display.Copies() == 1 && f.display.Matches(f.frame));
    CHECK(backend.draws == 1 && backend.clears == clears && backend.readbacks == reads);
    CHECK(backend.presents == 0 && backend.last_params.color.id != color.id);
    CHECK(!backend.last_params.do_frame_clear && !backend.last_params.clear_color &&
          !backend.last_params.has_32bit_content && backend.last_params.scale == 2);
    CHECK(backend.At(color).rgba == original_color && backend.At(depth).depth == original_depth);
    CHECK(source->coherence.needs_frame_clear && source->coherence.ever_composited);
    gs::DrawPipeline::Frame capture;
    CHECK(f.display.Present(true,&capture));
    CHECK(backend.presents == 1 && capture.grabbed && capture.source_fbp == 0);
    CHECK(f.presenter.EeFrameCount() == 1 && f.presenter.FrameCount() == 1);
    const auto shown = backend.last_present.source;
    CHECK(backend.last_present.filter == render::PresentationFilter::Linear);
    CHECK(shown.id != color.id && capture.w == 1024 && capture.h == 896);
    CHECK(f.display.Present(false,nullptr)); // held field, no new copy/readback
    CHECK(backend.presents == 2 && backend.draws == 1 && backend.readbacks == reads+1);
    CHECK(f.presenter.EeFrameCount() == 1 && f.presenter.FrameCount() == 2);
    CHECK(f.display.Copies() == 1 && backend.last_present.source.id == shown.id);
    auto stale = f.frame; ++stale.epoch;
    CHECK(!f.display.Matches(stale));
    auto bad = f.intent; bad.draw.scanmsk = 1;
    CHECK(f.display.Resolve(bad,f.frame) == Display::Resolution::Refused && backend.draws == 1);
    f.display.Clear();
    CHECK(!f.display.Active() && !f.display.Present(false,nullptr));
    f.scene.CollectAfterQuiescence();
    CHECK(!backend.Has(shown) && backend.Has(color));
}

void Quantization(render::GlDevice& backend,uint8_t scale) {
    TestGraphicsDisplay f(backend,scale);
    f.Source();
    const auto* scene = f.cache.Find(f.frame.color_buffer);
    const auto source = scene->views.color;
    const auto depth = scene->views.depth;
    const auto desc = backend.DescribeSurface(source);
    std::vector<uint8_t> before(size_t(desc.width)*desc.height*4),after(before.size());
    std::vector<float> depth_before(size_t(desc.width)*desc.height),depth_after(depth_before.size());
    backend.Readback(source,{0,0,int(desc.width),int(desc.height)},before.data(),desc.width*4);
    backend.Readback(depth,{0,0,int(desc.width),int(desc.height)},depth_before.data(),desc.width*4);
    CHECK(f.display.Resolve(f.intent,f.frame) == Display::Resolution::Ok);
    gs::DrawPipeline::Frame capture;
    CHECK(f.display.Present(true,&capture));
    CHECK(capture.w == 512*scale && capture.h == 448*scale);
    constexpr int matrix[16]{-4,2,-3,3,0,-2,1,-1,-3,3,-4,2,1,-1,0,-2};
    constexpr int color[3]{47,71,135};
    size_t differences = 0;
    for (int y = 0; y < capture.h; ++y) for (int x = 0; x < capture.w; ++x) {
        const int d = matrix[(y/scale%4)*4+(x/scale%4)];
        const auto at = (size_t(y)*capture.w+x)*4;
        for (int c = 0; c < 3; ++c)
            differences += capture.rgba[at+c] != uint8_t(std::clamp(color[c]+d,0,255)&~7);
        differences += capture.rgba[at+3] != 0;
    }
    CHECK(differences == 0); // OLD native-coordinate dither and zero-expanded 5551 store
    backend.Readback(source,{0,0,int(desc.width),int(desc.height)},after.data(),desc.width*4);
    backend.Readback(depth,{0,0,int(desc.width),int(desc.height)},depth_after.data(),desc.width*4);
    CHECK(before == after && depth_before == depth_after); // source alpha/depth are unchanged
    CHECK(glGetError() == GL_NO_ERROR);
}
}

int main(int argc,char** argv) {
    if (argc > 1 && std::strcmp(argv[1],"--gl") == 0) {
        platform::Window window;
        platform::GlContext context;
        if (!window.Create(true,1024,896,"Graphics display") || !context.Create(window)) return 77;
        render::GlDevice backend;
        CHECK(backend.Init([&] { window.Swap(); }));
        backend.SetWindowSize(1024,896);
        for (uint8_t scale : {1,2}) Quantization(backend,scale);
        backend.Shutdown();
    } else {
        LifetimeAndCadence();
    }
    std::printf("GraphicsDisplay: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
