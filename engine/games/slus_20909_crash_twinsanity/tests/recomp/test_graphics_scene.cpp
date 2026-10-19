#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_presentation.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "tests/lib/fake_backend.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/hle/queue/draw_ir_thread_queue.h"

namespace {
using namespace ps2r;
using Graphics = title::Graphics;
using Scene = title::GraphicsScene;
using Targets = title::GraphicsTargets;
using Resources = title::GraphicsResources;
using Surfaces = title::GraphicsSurfaces;
using Renderer = title::GraphicsRenderer;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(false)
class TestGraphicsScene {
public:
    struct Backend : test::FakeBackend {
        struct Call { gs::DrawIr draw; gs::TextureSource texture; render::DrawParams params; };
        std::vector<Call> calls;
        std::vector<std::thread::id> destroy_threads;
        bool fail_draw = false;
        render::DrawResult Draw(const gs::DrawIr& draw, const gs::TextureSource& texture,
                                const render::DrawParams& params) override {
            if (fail_draw) throw std::runtime_error("draw failure");
            calls.push_back({draw,texture,params});
            if (params.do_frame_clear)
                Clear(params.color, params.depth, 0, 0,
                      render::kClearDepth | (params.clear_color ? uint32_t{render::kClearColor} : 0u));
            return FakeBackend::Draw(draw,texture,params);
        }
        void DestroySurface(render::SurfaceId surface) override {
            destroy_threads.push_back(std::this_thread::get_id());
            FakeBackend::DestroySurface(surface);
        }
    };

    Backend backend;
    Surfaces surfaces{backend};
    gs::VramImage memory;
    gs::VramLayout layout;
    gs::ScalePolicy scale{gs::ScaleSettings{2}, &layout};
    gs::RenderTargetCache cache{surfaces,memory,scale};
    gs::TargetRefresher refresher{surfaces,memory,cache};
    gs::TextureResolver resolver{surfaces,memory,cache,refresher,layout};
    gs::ScanoutComposer scanout{surfaces,cache};
    gs::Presenter presenter{scanout};
    gs::SequenceRecipe sequence{gs::SequenceRecipe::Constants{}};
    gs::DrawPipeline pipeline{gs::DrawPipeline::Deps{
        &surfaces,&memory,&cache,&refresher,&resolver,&scanout,&presenter,&layout,nullptr,&sequence}};
    Resources resources{surfaces};
    Renderer renderer{surfaces,resources};
    Scene scene{surfaces,resources,renderer,pipeline};
    Graphics queue;
    Targets targets{queue};
    Scene::Frame frame{0x70000,0x150000,512,448,7,31};
    Scene::Binding binding;
    TestGraphicsScene() {
        pipeline.OnSetWindow(512,448,512,448);
        pipeline.OnSetFrontBuffer(frame.color_buffer,512,0);
    }
    Graphics::Hooks Hooks() {
        Graphics::Hooks hooks;
        hooks.draw = [&](const gs::DrawIr& draw, const std::vector<Graphics::Lease>& bindings) {
            return scene.Draw(draw,bindings) == Scene::Result::Ok;
        };
        hooks.upload = [&](const Graphics::Upload& upload) {
            return resources.Upload(upload) == Resources::Result::Ok;
        };
        hooks.drain = [] { return true; };
        return hooks;
    }
};
using World = TestGraphicsScene;
gs::DrawIr Triangle(const Scene::Frame& frame) {
    gs::DrawIr draw;
    draw.src_frame = frame.source_frame;
    draw.vram_epoch = frame.epoch;
    draw.prim_type = 3;
    draw.raster.frame = {frame.color_buffer, frame.width, 0, 0};
    draw.raster.zbuf = {frame.depth_buffer,0,false};
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 2;
    draw.raster.scissor = {0,0,uint16_t(frame.width-1),uint16_t(frame.height-1)};
    draw.verts.resize(3); draw.vertex_count = 3;
    draw.verts[1].x = 16; draw.verts[2].y = 16;
    return draw;
}
void TestMixedScene() {
    World world;
    world.pipeline.SetGfxEpoch(9001); // retained upload domain, unrelated to source setup generation
    auto draw = Triangle(world.frame);
    const std::vector<uint8_t> original(world.memory.Bytes(),world.memory.Bytes()+gs::VramImage::kBytes);
    CHECK(world.pipeline.DrawGeneric(draw).drew);
    const auto first = world.backend.calls.back().params;
    CHECK(first.do_frame_clear && first.clear_color);
    auto* target = world.cache.Find(world.frame.color_buffer);
    CHECK(target && target->coherence.main_author_seq == 1);
    CHECK(world.scene.Bind(world.queue,world.targets,world.frame,world.binding) == Scene::Result::Ok);
    CHECK(world.queue.BeginFrame(world.frame.source_frame) == Graphics::Result::Ok);
    draw.raster.fba = true;
    draw.alpha_blend = true;
    draw.raster.alpha = {2,0,0,1,0}; // OLD subtractive path remains explicit
    CHECK(world.targets.Record(world.queue.End(24),draw) == Targets::Result::Ok);
    CHECK(world.queue.Submit(world.Hooks()) == Graphics::Result::Ok);
    CHECK(world.pipeline.DrawSeq() == 2 && target->coherence.main_author_seq == 2);
    CHECK(world.pipeline.GfxEpoch() == 9001 && target->coherence.last_draw_gfx_epoch == 9001);
    CHECK(target->coherence.last_z_seq == 2 && target->coherence.last_zbp == world.frame.depth_buffer);
    CHECK(target->coherence.has_content && !target->coherence.drawn_since_sync.Empty());
    CHECK(target->coherence.cd_copy.dirty_x1 == 16 && target->coherence.cd_copy.dirty_y1 == 16);
    CHECK(world.backend.calls.back().params.color.id == first.color.id);
    CHECK(world.backend.calls.back().params.depth.id == first.depth.id);
    CHECK(!world.backend.calls.back().params.do_frame_clear);
    CHECK(world.backend.calls.back().params.scale == 2);
    CHECK(world.backend.calls.back().params.cd_copy == nullptr);
    CHECK(world.backend.calls.back().draw.raster.fba && world.backend.calls.back().draw.raster.alpha.a == 2);
    CHECK(world.pipeline.DrawGeneric(draw).drew);
    CHECK(world.pipeline.DrawSeq() == 3 && target->coherence.main_author_seq == 3);
    CHECK(world.backend.calls.back().params.color.id == first.color.id &&
          world.backend.calls.back().params.depth.id == first.depth.id);
    CHECK(!world.backend.calls.back().params.do_frame_clear); // typed draw wasn't erased
    CHECK(world.cache.MostRecentDrawnFbp() == world.frame.color_buffer);
    CHECK(world.backend.creates == 3 && world.resources.Counts().created == 0);
    CHECK(world.backend.readbacks == 0 && world.backend.uploads == 0 && world.backend.presents == 0);
    CHECK(std::memcmp(original.data(),world.memory.Bytes(),original.size()) == 0);
    world.targets.Reset();
    Scene::Release(world.queue,world.binding);
    world.scene.CollectAfterQuiescence();
    CHECK(world.resources.Counts().live_bindings == 0 && world.backend.Has(first.color));
}

void TestOwnedPresentation() {
    World world;
    title::GraphicsPresentation presentation(world.scene, world.resources, world.pipeline);
    CHECK(!presentation.Select(world.frame)); // no parallel blank scene allocation
    CHECK(world.backend.creates == 0);
    CHECK(world.pipeline.DrawGeneric(Triangle(world.frame)).drew);
    auto* scene = world.cache.Find(world.frame.color_buffer);
    const auto exact_color = scene->views.color;
    CHECK(presentation.Select(world.frame));
    // OLD FMV3: a stale display/sky choice must not steal the selected scene.
    world.pipeline.OnSetFrontBuffer(0x2c0000,512,0);
    const auto bytes = std::vector<uint8_t>(world.memory.Bytes(),
        world.memory.Bytes() + gs::VramImage::kBytes);
    const int clears = world.backend.clears, readbacks = world.backend.readbacks;
    unsigned crtc_reads = 0;
    gs::DrawIrQueueConsumer::Hooks hooks;
    hooks.crtc = [&] { ++crtc_reads; return gs::CrtcClassifier::Snapshot{}; };
    hooks.owned_frame = [&](bool frame, auto* capture) { return presentation.Present(frame,capture); };
    gs::DrawIrQueueConsumer consumer(world.pipeline,hooks);
    gs::JobRec boundary{};
    boundary.type = uint32_t(gs::JobKind::VSync);
    boundary.arg1 = 1;
    consumer.OnFrame(boundary);
    CHECK(world.backend.presents == 1 && world.backend.clears == clears);
    CHECK(world.backend.last_present.source.id == exact_color.id);
    CHECK(world.presenter.EeFrameCount() == 1 && world.presenter.FrameCount() == 1);
    CHECK(consumer.LastFrame().grabbed && !consumer.LastFrame().source_fallback);
    CHECK(consumer.LastFrame().source_fbp == world.frame.color_buffer);
    CHECK(world.backend.readbacks == readbacks + 1 && crtc_reads == 0);
    const auto captured = consumer.LastFrame().rgba;
    boundary.arg1 = 2; // real display tick: hold image, no extra grab/game frame
    consumer.OnFrame(boundary);
    CHECK(world.backend.presents == 2 && presentation.Presents() == 2);
    CHECK(world.presenter.EeFrameCount() == 1 && world.presenter.FrameCount() == 2);
    CHECK(consumer.LastFrame().rgba == captured && world.backend.readbacks == readbacks + 1);
    CHECK(std::memcmp(bytes.data(),world.memory.Bytes(),bytes.size()) == 0); // no VRAM deposit
    CHECK(world.backend.clears == clears && crtc_reads == 0);
    // A selected failure cannot secretly replay retained presentation.
    hooks.owned_frame = [](bool, auto*) -> bool { throw std::runtime_error("selected failure"); };
    gs::DrawIrQueueConsumer failing(world.pipeline,hooks);
    bool threw = false;
    try { failing.OnFrame(boundary); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && world.backend.presents == 2 && crtc_reads == 0);
    presentation.Clear();
    CHECK(!presentation.Active() && !presentation.Present(false,nullptr));
}
void TestTypedFirstAndFrameClear() {
    World world;
    CHECK(world.scene.Bind(world.queue,world.targets,world.frame,world.binding) == Scene::Result::Ok);
    CHECK(world.queue.BeginFrame(world.frame.source_frame) == Graphics::Result::Ok);
    auto draw = Triangle(world.frame);
    CHECK(world.targets.Record(world.queue.End(0),draw) == Targets::Result::Ok);
    CHECK(world.queue.Submit(world.Hooks()) == Graphics::Result::Ok);
    CHECK(world.backend.calls.back().params.do_frame_clear && world.backend.calls.back().params.clear_color);
    CHECK(world.pipeline.DrawGeneric(draw).drew);
    CHECK(!world.backend.calls.back().params.do_frame_clear);
    // Same source EE boundary advances the existing clear rule once.
    world.presenter.MarkEeFrameEnd();
    CHECK(world.queue.BeginFrame(world.frame.source_frame) == Graphics::Result::Ok);
    CHECK(world.targets.Record(world.queue.End(0),draw) == Targets::Result::Ok);
    CHECK(world.queue.Submit(world.Hooks()) == Graphics::Result::Ok);
    CHECK(world.backend.calls.back().params.do_frame_clear);
    CHECK(world.pipeline.DrawGeneric(draw).drew);
    CHECK(!world.backend.calls.back().params.do_frame_clear);
    CHECK(world.backend.creates == 3 && world.backend.presents == 0 && world.backend.readbacks == 0);
}
void TestResizePinAndStaleBinding() {
    World world;
    CHECK(world.scene.Bind(world.queue,world.targets,world.frame,world.binding) == Scene::Result::Ok);
    const auto old = world.cache.Find(world.frame.color_buffer)->views.color;
    Graphics::Lease external;
    CHECK(world.queue.RetainResource(world.binding.handles[0],&external) == Graphics::Result::Ok);
    CHECK(world.queue.BeginFrame(world.frame.source_frame) == Graphics::Result::Ok);
    CHECK(world.targets.Record(world.queue.End(0),Triangle(world.frame)) == Targets::Result::Ok);
    auto& larger = world.cache.GetOrCreate(world.frame.color_buffer,576,448,0,32);
    CHECK(larger.views.color.id != old.id);
    CHECK(world.backend.Has(old) && world.surfaces.PendingRetirements() == 3);
    CHECK(!world.surfaces.Pin(old)); // no new consumer can acquire a retired version
    CHECK(world.queue.Submit(world.Hooks()) == Graphics::Result::DispatchFailed);
    CHECK(world.backend.calls.empty()); // do not write a version the retained scene no longer uses
    CHECK(world.queue.Recover(world.Hooks()) == Graphics::Result::Ok);
    world.targets.Reset();
    Scene::Release(world.queue,world.binding);
    world.scene.CollectAfterQuiescence();
    CHECK(world.backend.Has(old));
    std::thread producer([&] { external.reset(); });
    producer.join();
    CHECK(world.backend.Has(old));
    world.scene.CollectAfterQuiescence();
    CHECK(!world.backend.Has(old) && world.surfaces.PendingRetirements() == 0);
    for (auto thread : world.backend.destroy_threads) CHECK(thread == std::this_thread::get_id());
}
void TestUnclosedDependenciesAndFailure() {
    for (unsigned variant = 0; variant < 6; ++variant) {
        World world;
        CHECK(world.scene.Bind(world.queue,world.targets,world.frame,world.binding) == Scene::Result::Ok);
        CHECK(world.queue.BeginFrame(world.frame.source_frame) == Graphics::Result::Ok);
        auto draw = Triangle(world.frame);
        if (variant == 5) {
            draw.raster.test.date = true;
            draw.raster.zbuf.zmsk = true;
            world.cache.GetOrCreate(world.frame.depth_buffer,512,448,0,31);
        }
        CHECK(world.targets.Record(world.queue.End(0),draw) == Targets::Result::Ok);
        if (variant == 0) world.cache.AliasPages().MarkAliased(world.frame.color_buffer,8192);
        if (variant == 1) world.cache.AliasPages().MarkCpuDirty(world.frame.color_buffer,8192);
        if (variant == 2) world.cache.Ledger().Record(world.frame.color_buffer,8192,1);
        if (variant == 3) world.backend.fail_draw = true;
        if (variant == 4) world.sequence.Prime(0x230000);
        CHECK(world.queue.Submit(world.Hooks()) == Graphics::Result::DispatchFailed);
        CHECK(world.backend.calls.empty());
        CHECK(world.backend.readbacks == 0 && world.backend.uploads == 0);
        CHECK(world.queue.Recover(world.Hooks()) == Graphics::Result::Ok);
        // Avoid asking the retained cache destructor to service this synthetic
        // unfinished ledger; this test is specifically the no-import gate.
        world.cache.Ledger().Remove(world.frame.color_buffer);
    }
}
void TestBindDoesNotRescue() {
    World world;
    CHECK(world.pipeline.DrawGeneric(Triangle(world.frame)).drew);
    const auto surface = world.cache.Find(world.frame.color_buffer)->views.color;
    world.frame.width += 64;
    CHECK(world.scene.Bind(world.queue,world.targets,world.frame,world.binding) ==
          Scene::Result::UnclosedDependency);
    CHECK(world.backend.readbacks == 0 && world.backend.uploads == 0);
    CHECK(world.cache.Find(world.frame.color_buffer)->views.color.id == surface.id);
}
void TestOrderedUploadCrossing() {
    // Actual consumer and ring, including BOTH pending-upload stages. Sync()
    // alone leaves these pending; the owned task must see the uploaded bytes
    // before drawing and must execute on the existing consumer owner.
    for (unsigned mode = 0; mode < 3; ++mode) {
        World world;
        std::vector<uint8_t> pixels(16*16*4,0x31);
        std::unique_ptr<gs::DrawIrThreadQueue> ring;
        gs::DrawIrQueueConsumer::Hooks consumer_hooks;
        consumer_hooks.upload_pixels = [&] {
            return ring ? ring->LastUploadPayload() : &pixels;
        };
        gs::DrawIrQueueConsumer consumer(world.pipeline,consumer_hooks);
        if (mode) {
            gs::DrawIrThreadQueue::Hooks hooks;
            hooks.upload_pixels = [&] { return &pixels; };
            ring = std::make_unique<gs::DrawIrThreadQueue>(
                consumer,hooks,mode == 1 ? gs::DrawIrThreadQueue::Policy::Lockstep :
                                         gs::DrawIrThreadQueue::Policy::Pipelined,2);
            ring->Start();
        }
        auto* sink = ring ? static_cast<gs::DrawIrSink*>(ring.get()) : &consumer;
        gs::JobRec upload{};
        upload.type = uint32_t(gs::JobKind::UploadVRAMTexture);
        upload.arg1 = 0x300000;
        upload.arg2 = upload.arg3 = 16;
        upload.payload_bytes = uint32_t(pixels.size());
        sink->OnJob(upload);
        std::fill(pixels.begin(),pixels.end(),0x63); // published after OnJob returns
        if (ring) ring->Sync();
        CHECK(world.backend.uploads == 0); // the negative control
        const auto producer_thread = std::this_thread::get_id();
        std::thread::id draw_thread;
        auto owned = [&] {
            return consumer.RunOrdered([&] {
                draw_thread = std::this_thread::get_id();
                const auto* image = world.refresher.FindUploaded(upload.arg1);
                CHECK(image && world.backend.uploads == 1);
                if (image) CHECK(world.backend.At(image->tex).rgba == pixels);
                CHECK(world.scene.Bind(world.queue,world.targets,world.frame,world.binding) == Scene::Result::Ok);
                CHECK(world.queue.BeginFrame(world.frame.source_frame) == Graphics::Result::Ok);
                CHECK(world.targets.Record(world.queue.End(0),Triangle(world.frame)) == Targets::Result::Ok);
                return world.queue.Submit(world.Hooks()) == Graphics::Result::Ok;
            });
        };
        CHECK(ring ? ring->RunOrdered(owned) : owned());
        CHECK((draw_thread != producer_thread) == bool(mode));
        CHECK(consumer.Count().uploads == 1 && consumer.Count().uploads_dropped == 0);
        const auto shared_color = world.backend.calls.back().params.color;
        sink->OnDraw(Triangle(world.frame));
        if (ring) ring->Sync();
        CHECK(world.backend.calls.back().params.color.id == shared_color.id);
        CHECK(!world.backend.calls.back().params.do_frame_clear);
        CHECK(world.pipeline.DrawSeq() == 2);
        CHECK(world.backend.presents == 0 && world.backend.readbacks == 0);
        if (ring) {
            CHECK(!ring->RunOrdered([]() -> bool { throw std::runtime_error("selected failure"); }));
            CHECK(ring->RunOrdered([] { return true; })); // exception cannot kill the owner
            ring->Stop();
        }
    }
}
} // namespace
int main() {
    TestMixedScene();
    TestTypedFirstAndFrameClear();
    TestResizePinAndStaleBinding();
    TestUnclosedDependenciesAndFailure();
    TestBindDoesNotRescue();
    TestOrderedUploadCrossing();
    TestOwnedPresentation();
    std::printf("GraphicsScene: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
