#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <cstdio>
#include <cstring>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "tests/lib/fake_backend.h"

namespace {
using namespace ps2r;
using Scene = title::GraphicsScene;
using Intent = title::FrameClear;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)
class TestGraphicsFrameClear {
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
    Scene::Frame frame{0,8192,64,32,7,31};
    explicit TestGraphicsFrameClear(render::Backend& backend,uint8_t factor)
        : surfaces(backend),scale(gs::ScaleSettings{factor},&layout) {
        pipeline.OnSetWindow(64*factor,32*factor,64,32);
        pipeline.OnSetFrontBuffer(frame.color_buffer,64,0);
    }
    Intent::Output Clear(unsigned mask,bool full = false) {
        Intent::Output out;
        Intent::Input in{2016u+(full ? 0u : 8u),2032u+(full ? 0u : 6u),
                        full ? 64u : 47u,full ? 32u : 7u,64,32,
                        17,199,253,bool(mask&1),bool(mask&2)};
        CHECK(Intent::Build(in,out) == Intent::Result::Ok);
        return out;
    }
    gs::RenderTarget& Seed() {
        auto& target = cache.GetOrCreate(frame.color_buffer,64,32,0,0);
        surfaces.Clear(target.views.color,target.views.depth,0xb687472fu,0.75f);
        target.coherence.valid_rect.Union(0,0,64,32);
        target.coherence.has_content = true;
        target.coherence.last_z_wrote = true;
        target.coherence.last_zbp = frame.depth_buffer;
        target.coherence.last_z_seq = 11;
        pipeline.RestoreDrawOrder(11);
        target.coherence.needs_frame_clear = target.coherence.ever_composited = true;
        target.identity.render_fbw = 64;
        target.identity.has_32bit_content = target.identity.was_32bit = true;
        return target;
    }
    title::SceneState::Snapshot Snapshot() {
        title::SceneState::Snapshot snapshot;
        std::string error;
        CHECK(scene.CaptureState(snapshot,&error));
        if (!error.empty()) std::fprintf(stderr,"%s\n",error.c_str());
        return snapshot;
    }
};

void OrderingAndRefusal() {
    for (unsigned mask = 0; mask < 4; ++mask) {
        test::FakeBackend backend;
        TestGraphicsFrameClear f(backend,2);
        const auto partial = f.Clear(mask);
        CHECK(f.scene.StartFrame(partial,f.frame) == Scene::Result::UnclosedDependency);
        CHECK(!backend.creates && !f.scene.FrameSetups()); // no blank inherited scene
        auto& target = f.Seed();
        const auto color = target.views.color,depth = target.views.depth;
        const int clears = backend.clears,reads = backend.readbacks,uploads = backend.uploads;
        const auto mirror = std::vector<uint8_t>(f.memory.Bytes(),f.memory.Bytes()+gs::VramImage::kBytes);
        CHECK(f.scene.StartFrame(partial,f.frame) == Scene::Result::Ok);
        CHECK(f.scene.FrameSetups() == 1 && backend.draws == 1);
        CHECK(backend.last_params.color.id == color.id && backend.last_params.depth.id == depth.id);
        CHECK(!backend.last_params.do_frame_clear && !backend.last_params.clear_color);
        CHECK(backend.clears == clears && backend.readbacks == reads && backend.uploads == uploads);
        CHECK(!backend.presents && !target.WantsFrameClear(f.presenter.EeFrameCount()));
        CHECK(bool(target.coherence.last_z_seq != 11) == bool(mask&2));
        auto next = partial.draw;
        next.raster.frame.fbmsk = UINT32_MAX; next.raster.zbuf.zmsk = true;
        CHECK(f.pipeline.DrawGeneric(next).drew);
        CHECK(!backend.last_params.do_frame_clear && backend.clears == clears);
        CHECK(std::memcmp(mirror.data(),f.memory.Bytes(),mirror.size()) == 0);
        // A deferred writeback still owing this buffer's rows (the native
        // present's ledger entry) is deposited BEFORE the owned clear -- the
        // retained arm's own pre-draw rule (DrawPipeline::SameBufferPreDrawFlush)
        // -- never an unclosed dependency: one readback, the entry gone, the
        // frame set up. An entry on ANOTHER buffer stays deferred.
        f.cache.Ledger().Record(f.frame.color_buffer,8192,1);
        f.cache.Ledger().Record(0x3f0000,8192,1);   // another buffer, outside this target
        const auto count = backend.draws;
        const auto settles = f.scene.PreDrawSettles();
        CHECK(f.scene.StartFrame(partial,f.frame) == Scene::Result::Ok);
        CHECK(backend.draws == count + 1 && f.scene.FrameSetups() == 2 && backend.readbacks == reads + 1);
        CHECK(f.scene.PreDrawSettles() == settles + 1 && !f.cache.Ledger().Has(f.frame.color_buffer) &&
              f.cache.Ledger().Has(0x3f0000) &&
              f.cache.Ledger().Flushes(gs::PendingRtSyncLedger::Reason::PreDraw) == 1);
        f.cache.Ledger().Remove(0x3f0000);
        auto empty = partial;
        empty.draw.verts.clear(); empty.draw.vertex_count = 0;
        CHECK(f.scene.StartFrame(empty,f.frame) == Scene::Result::Ok);
        CHECK(backend.draws == count + 1 && f.scene.FrameSetups() == 3);
        // The frame-62 shape of the all-on coldboot: the native scanout of the
        // frame this target just drew WROTE IT BACK (AliasPageSet marks the
        // footprint cpu_dirty + aliased, VramImage records this target as the
        // pages' author). The target's own deposit is the attachment's own
        // content: not a dependency -- the next owned frame is set up. The same
        // pages last authored by the EE (an upload under the frame) ARE one.
        const uint32_t fp = gs::RenderTargetCache::FootprintBytes(*f.cache.Find(f.frame.color_buffer));
        f.cache.AliasPages().MarkCpuDirty(f.frame.color_buffer,fp);
        f.cache.AliasPages().MarkAliased(f.frame.color_buffer,fp);
        f.memory.NoteGpuAuthor(gs::VramImage::Span{f.frame.color_buffer,fp},f.frame.color_buffer);
        CHECK(!f.scene.ForeignPages(f.frame,fp));
        CHECK(f.scene.StartFrame(empty,f.frame) == Scene::Result::Ok);
        CHECK(f.scene.FrameSetups() == 4 && backend.readbacks == reads + 1 && backend.uploads == uploads);
        f.memory.NoteGpuAuthor(gs::VramImage::Span{f.frame.color_buffer,fp},0x3f0000); // another target's deposit
        CHECK(f.scene.ForeignPages(f.frame,fp));
        CHECK(f.scene.StartFrame(empty,f.frame) == Scene::Result::UnclosedDependency);
        f.memory.NoteCpuWrite(gs::VramImage::Span{f.frame.color_buffer,fp});            // an EE upload under the frame
        CHECK(f.scene.ForeignPages(f.frame,fp));
        CHECK(f.scene.StartFrame(empty,f.frame) == Scene::Result::UnclosedDependency);
        CHECK(f.scene.FrameSetups() == 4);
        f.cache.AliasPages().ClearCpuDirty(f.frame.color_buffer,fp);
        f.cache.AliasPages().ClearAliased(f.frame.color_buffer,fp);
        CHECK(!f.scene.ForeignPages(f.frame,fp) && f.scene.StartFrame(empty,f.frame) == Scene::Result::Ok);
    }
    test::FakeBackend backend;
    TestGraphicsFrameClear fresh(backend,1);
    CHECK(fresh.scene.StartFrame(fresh.Clear(3,true),fresh.frame) == Scene::Result::Ok);
    CHECK(backend.creates == 3 && backend.draws == 1);
    CHECK(!backend.last_params.do_frame_clear && !backend.presents && !backend.readbacks);
}

void PixelsAndRestore(render::GlDevice& backend,uint8_t factor,unsigned mask) {
    TestGraphicsFrameClear f(backend,factor);
    f.Seed();
    const auto before = f.Snapshot();
    const auto intent = f.Clear(mask);
    CHECK(f.scene.StartFrame(intent,f.frame) == Scene::Result::Ok);
    const auto after = f.Snapshot();
    CHECK(after.targets.size() == 1 && before.targets.size() == 1);
    if (after.targets.size() != 1 || before.targets.size() != 1) return;
    const auto& pixels = after.targets[0].color;
    const auto& depth = after.targets[0].depth;
    size_t differences = 0;
    for (unsigned y = 0; y < 32*factor; ++y) for (unsigned x = 0; x < 64*factor; ++x) {
        const bool covered = x >= 8u*factor && x < 40u*factor && y >= 6u*factor && y < 13u*factor;
        const size_t at = size_t(y)*64*factor+x;
        constexpr uint8_t clear[4]{17,199,253,0};
        for (size_t c = 0; c < 4; ++c)
            differences += pixels[at*4+c] != ((covered && (mask&1)) ? clear[c] :
                                                                 before.targets[0].color[at*4+c]);
        differences += depth[at] != ((covered && (mask&2)) ? 0.0f : before.targets[0].depth[at]);
    }
    CHECK(differences == 0); // exact physical-scale masks, alpha and truncating rectangle
    CHECK(after.targets[0].history.last_clear_frame == f.presenter.EeFrameCount() &&
          !after.targets[0].history.needs_frame_clear);
    // Put the incoming contents into a fresh owner. The next masked retained
    // draw must not erase the preserved alpha/depth outside the source rect.
    TestGraphicsFrameClear restored(backend,factor);
    CHECK(restored.scene.StageState(after) && restored.scene.PublishState());
    auto next = intent.draw;
    next.raster.frame.fbmsk = UINT32_MAX; next.raster.zbuf.zmsk = true;
    CHECK(restored.pipeline.DrawGeneric(next).drew);
    const auto continuation = restored.Snapshot();
    CHECK(continuation.targets.size() == 1);
    if (continuation.targets.size() == 1) {
        CHECK(continuation.targets[0].color == pixels);
        CHECK(continuation.targets[0].depth == depth);
    }
    CHECK(glGetError() == GL_NO_ERROR);
}
}
int main(int argc,char** argv) {
    if (argc > 1 && std::strcmp(argv[1],"--gl") == 0) {
        platform::Window window;
        platform::GlContext context;
        if (!window.Create(true,128,64,"Graphics source clear") || !context.Create(window)) return 77;
        render::GlDevice backend;
        CHECK(backend.Init([&] { window.Swap(); }));
        backend.SetWindowSize(128,64);
        for (uint8_t scale : {1,2})
            for (unsigned mask = 0; mask < 4; ++mask) PixelsAndRestore(backend,scale,mask);
        backend.Shutdown();
    } else OrderingAndRefusal();
    std::printf("GraphicsFrameClear: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
