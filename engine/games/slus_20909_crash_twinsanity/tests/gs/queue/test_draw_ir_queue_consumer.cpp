// tests/gs/queue/test_draw_ir_queue_consumer.cpp -- DrawIrQueueConsumer over the
// FakeBackend: every JobKind reaches its DrawPipeline entry in stream order, the
// upload payload is applied at the next event (before the draw that follows),
// OnFrame runs OnFrameEnd (the grab lands, the present is skipped until the
// first DISPFB job set the scanout base), the stamps and the hooks.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/gs/hle/classrdr/class_registry.h"
#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "engine/gs/hle/classrdr/sequence_recipe.h"
#include "engine/gs/hle/ir/job_rec.h"
#include "engine/gs/hle/present/presenter.h"
#include "engine/gs/hle/present/scanout_composer.h"
#include "engine/core/verify/frame_hasher.h"
#include "engine/gs/hle/queue/draw_ir_queue_consumer.h"
#include "engine/gs/hle/targets/render_target_cache.h"
#include "engine/gs/hle/targets/scale_policy.h"
#include "engine/gs/hle/targets/target_refresher.h"
#include "engine/gs/hle/texsrc/texture_resolver.h"
#include "engine/gs/vram/vram_image.h"
#include "games/slus_20909_crash_twinsanity/renderer_profile/renderer_profile.h"
#include "tests/lib/fake_backend.h"

using namespace ps2r;
using namespace ps2r::gs;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);       \
        }                                                                   \
    } while (0)

namespace {

const title::RendererProfile& Profile() {
    static const title::RendererProfile p;
    return p;
}

// The replay driver's world at scale 1 (no renderers registered: generic lowering).
struct World {
    test::FakeBackend be;
    VramImage vram;
    ScalePolicy scale{ScaleSettings{1}, &Profile().Vram()};
    RenderTargetCache cache{be, vram, scale};
    TargetRefresher refresher{be, vram, cache};
    TextureResolver resolver{be, vram, cache, refresher, Profile().Vram()};
    ScanoutComposer scanout{be, cache};
    Presenter presenter{scanout};
    ClassRegistry registry{Profile().ClassRules()};
    SequenceRecipe sequence{Profile().Sequence()};
    DrawPipeline pipe{DrawPipeline::Deps{&be, &vram, &cache, &refresher, &resolver, &scanout, &presenter,
                                          &Profile().Vram(), &registry, &sequence}};
};

// The live rule the host applies at construction: no scanout base until the first DISPFB job.
void LiveRules(World& w) { w.scanout.SetScanoutFbp(DrawPipeline::kNoFbp); }

void AddVert(DrawIr& ir, float x, float y) {
    VertexRec r{};
    r.x = x; r.y = y; r.z = 0; r.q = 1.0f;
    r.r = r.g = r.b = r.a = 0x80;
    ir.verts.push_back(r);
}

DrawIr Triangle(uint32_t src_frame, uint64_t epoch) {
    DrawIr ir;
    ir.prim_type = 3;
    ir.raster.frame.fbp = 0x70000; ir.raster.frame.fbw = 512; ir.raster.frame.psm = 0;
    ir.raster.scissor = DrawIr::Scissor{0, 0, 511, 447};
    ir.src_frame = src_frame;
    ir.vram_epoch = epoch;
    AddVert(ir, 0, 0); AddVert(ir, 8, 0); AddVert(ir, 0, 8);
    return ir;
}

JobRec Job(JobKind k, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    JobRec j{};
    j.type = static_cast<uint32_t>(k);
    j.arg1 = a1; j.arg2 = a2; j.arg3 = a3; j.arg4 = a4;
    j.src_frame = 5;
    j.vram_epoch = 77;
    return j;
}

struct Trace {
    std::vector<std::string> events;
    std::vector<uint8_t> pixels;
    int resized_w = 0, resized_h = 0, resizes = 0, enters = 0, crtc_calls = 0;
};

DrawIrQueueConsumer::Hooks HooksFor(Trace& t) {
    DrawIrQueueConsumer::Hooks h;
    h.enter = [&t] { t.enters++; };
    h.upload_pixels = [&t] { t.events.push_back("pixels"); return &t.pixels; };
    h.crtc = [&t] {
        t.crtc_calls++;
        CrtcClassifier::Snapshot s;
        s.valid = true;
        return s;
    };
    h.window_resized = [&t](int w, int h) { t.resizes++; t.resized_w = w; t.resized_h = h; };
    h.handler_name = [](uint32_t id) { return id == 7 ? "vs_family_tristrip" : nullptr; };
    return h;
}

}  // namespace

// ---- the job mapping ---------------------------------------------------------
static void TestMapping() {
    World w;
    LiveRules(w);
    Trace t;
    DrawIrQueueConsumer c(w.pipe, HooksFor(t));
    // SetWindow: arg2 = window_w<<16 | content_w, arg3 = window_h<<16 | content_h.
    c.OnJob(Job(JobKind::SetWindow, 1, (1280u << 16) | 640u, (896u << 16) | 448u, 0));
    CHECK(w.pipe.WindowW() == 1280 && w.pipe.WindowH() == 896, "SetWindow -> OnSetWindow window dims");
    CHECK(w.scanout.ContentW() == 640 && w.scanout.ContentH() == 448, "SetWindow -> content dims");
    CHECK(t.resizes == 1 && t.resized_w == 1280 && t.resized_h == 896, "window size change -> window_resized hook");
    c.OnJob(Job(JobKind::SetWindow, 1, (1280u << 16) | 640u, (896u << 16) | 448u, 0));
    CHECK(t.resizes == 1, "same window size -> no resize hook");
    CHECK(w.pipe.GfxEpoch() == 77, "job stamps the gfx epoch from the record");
    // SetFrontBuffer: fbp bytes, fbw px, psm.
    CHECK(w.scanout.ScanoutFbp() == DrawPipeline::kNoFbp, "scanout base unset before the first DISPFB job");
    c.OnJob(Job(JobKind::SetFrontBuffer, 0x70000, 512, 0, 0));
    CHECK(w.scanout.ScanoutFbp() == 0x70000u, "SetFrontBuffer -> OnSetFrontBuffer scanout base");
    // InvalidateTextures: reaches OnInvalidate (no observable with an empty cache; must not throw).
    c.OnJob(Job(JobKind::InvalidateTextures, 0x100000, 0x8000, 0, 0));
    // UploadVRAMTexture: pending until the next event, then OnUpload with the published pixels.
    JobRec up = Job(JobKind::UploadVRAMTexture, 0x200000, 4, 2, 4 * 2 * 4);
    up.payload_bytes = 4 * 2 * 4;
    t.pixels.assign(up.payload_bytes, 0xAB);
    const int uploads_before = w.be.uploads;
    c.OnJob(up);
    CHECK(c.HasPendingUpload() && w.be.uploads == uploads_before, "upload held pending (pixels published after the push)");
    c.OnJob(Job(JobKind::SetClearColor, 1, 2, 3, 0));
    CHECK(!c.HasPendingUpload() && w.be.uploads == uploads_before + 1 && c.Count().uploads == 1,
          "the next event applies the upload first (OnUpload -> Backend::Upload)");
    CHECK(c.Count().uploads_dropped == 0, "nothing dropped");
    // A pending upload whose published copy is the wrong size is dropped and counted.
    c.OnJob(up);
    t.pixels.assign(3, 0);
    c.OnJob(Job(JobKind::SetBlendMode));
    CHECK(c.Count().uploads_dropped == 1 && w.be.uploads == uploads_before + 1, "size mismatch -> dropped, counted");
    // No-op kinds leave the backend alone.
    const int creates = w.be.creates, draws = w.be.draws;
    c.OnJob(Job(JobKind::SetScissor)); c.OnJob(Job(JobKind::SetDepthTest)); c.OnJob(Job(JobKind::DrawBatch));
    CHECK(w.be.creates == creates && w.be.draws == draws, "SetScissor / SetDepthTest / DrawBatch: no backend work");
    CHECK(c.Count().jobs == 11, "eleven jobs counted");
    CHECK(t.enters == 11, "enter hook once per event");
}

// ---- draws, the pending upload before the draw, SyncAll --------------------
static void TestDrawOrder() {
    World w;
    LiveRules(w);
    Trace t;
    DrawIrQueueConsumer c(w.pipe, HooksFor(t));
    JobRec up = Job(JobKind::UploadVRAMTexture, 0x200000, 2, 2, 16);
    up.payload_bytes = 16;
    t.pixels.assign(16, 0x11);
    c.OnJob(up);
    CHECK(w.be.uploads == 0 && w.be.draws == 0, "nothing reached the backend yet");
    c.OnDraw(Triangle(9, 1234));
    CHECK(w.be.uploads == 1 && w.be.draws == 1, "upload applied, then the draw");
    CHECK(t.events.size() == 1 && t.events[0] == "pixels", "the pixels were read once, at the drain");
    CHECK(w.pipe.GfxEpoch() == 1234, "draw stamps the gfx epoch from the record");
    CHECK(c.Count().draws == 1 && c.Count().uploads == 1, "counters");
    // A draw with an HLE identity carries its handler name (generic lowering without renderers).
    DrawIr hle = Triangle(9, 1234);
    hle.identity.hle_handler = 7;
    c.OnDraw(hle);
    CHECK(w.be.draws == 2, "named draw lowered");
    // SyncAll after draws: every target's writeback (Backend::Readback) runs.
    const int rb = w.be.readbacks;
    c.OnJob(Job(JobKind::SyncAllRTsToVRAM));
    CHECK(w.be.readbacks > rb, "SyncAllRTsToVRAM -> OnSyncAll -> target readback");
}

// ---- the frame boundary -----------------------------------------------------
static void TestFrame() {
    World w;
    LiveRules(w);
    Trace t;
    DrawIrQueueConsumer c(w.pipe, HooksFor(t));
    JobRec vs = Job(JobKind::VSync, 1);
    vs.src_frame = 3;
    // No target yet: the grab is empty, the present is skipped (no scanout base).
    c.OnFrame(vs);
    CHECK(c.Count().frames == 1 && c.Count().grab_empty == 1 && c.Count().presents == 0, "empty frame: no grab, no present");
    CHECK(!c.LastFrame().grabbed && w.be.presents == 0, "nothing presented without a scanout base");
    CHECK(w.presenter.EeFrameCount() == 1, "arg1 == 1 marked an EE frame");
    CHECK(t.crtc_calls == 1, "the CRTC snapshot was taken at the frame end");
    // A draw, then the frame: the grab falls back to the most recently drawn target
    // (the live rule before the first DISPFB) and still no present.
    c.OnDraw(Triangle(3, 1));
    c.OnFrame(vs);
    CHECK(c.LastFrame().grabbed && c.LastFrame().w > 0 && c.LastFrame().h > 0, "grab of the drawn target");
    CHECK(c.LastFrame().rgba.size() == static_cast<size_t>(c.LastFrame().w) * c.LastFrame().h * 4, "RGBA8 bytes");
    CHECK(w.be.presents == 0 && c.Count().presents == 0, "still no present before DISPFB");
    // DISPFB + DISPLAY then the frame: the present runs (Backend::Present once).
    c.OnJob(Job(JobKind::SetFrontBuffer, 0x70000, 512, 0, 0));
    c.OnJob(Job(JobKind::SetWindow, 1, (640u << 16) | 640u, (448u << 16) | 448u, 0));
    c.OnFrame(vs);
    CHECK(w.be.presents == 1 && c.Count().presents == 1, "DISPFB set -> OnFrameEnd presents");
    CHECK(c.LastFrame().grabbed, "the grab precedes the present");
    CHECK(w.presenter.FrameCount() == 3, "three frame boundaries ran through the presenter");
    JobRec not_ee = vs;
    not_ee.arg1 = 0;
    c.OnFrame(not_ee);
    CHECK(w.presenter.EeFrameCount() == 3, "arg1 == 0 does not mark an EE frame");
    CHECK(c.Count().frames == 4, "four frames counted");
}

// ---- the lane hash at the grab ------------------------------------------------
// With a hash scale set the consumer hashes its grab (FrameHasher::HashNativeScale
// at that scale) and the header copy carries it without the pixels: the
// wall-locked boundary's lane line equals the one taken over the pixels. Without a
// scale nothing is hashed (the deterministic boundary hashes the reference frame).
static void TestFrameHash() {
    World w;
    LiveRules(w);
    Trace t;
    DrawIrQueueConsumer c(w.pipe, HooksFor(t));
    JobRec vs = Job(JobKind::VSync, 1);
    c.OnDraw(Triangle(3, 1));
    c.OnFrame(vs);
    CHECK(c.LastFrame().grabbed && !c.LastFrame().hashed && c.LastFrame().hash == 0, "no hash scale: the grab is not hashed");
    DrawPipeline::Frame hdr0 = c.LastFrameHeaderCopy();
    CHECK(hdr0.rgba.empty() && hdr0.grabbed && hdr0.w == c.LastFrame().w && hdr0.h == c.LastFrame().h && !hdr0.hashed,
          "header copy: no pixels, the flags and dims");
    for (int scale = 1; scale <= 2; ++scale) {
        c.SetFrameHashScale(scale);
        c.OnDraw(Triangle(3, 1));
        c.OnFrame(vs);
        const DrawPipeline::Frame full = c.LastFrameCopy();
        const DrawPipeline::Frame hdr = c.LastFrameHeaderCopy();
        const uint64_t want = ps2r::verify::FrameHasher::HashNativeScale(full.rgba.data(), full.w, full.h, scale);
        CHECK(full.hashed && full.hash == want && want != 0, "the grab's hash == HashNativeScale over its pixels at the scale");
        CHECK(hdr.hashed && hdr.hash == want && hdr.rgba.empty() && hdr.grabbed && hdr.source == full.source &&
              hdr.source_fbp == full.source_fbp && hdr.source_fallback == full.source_fallback,
              "the header copy carries the same hash and identity, no pixels");
    }
    // A frame with nothing grabbable hashes 0 (the lane's "no readable scanout" line).
    World w2;
    LiveRules(w2);
    Trace t2;
    DrawIrQueueConsumer c2(w2.pipe, HooksFor(t2));
    c2.SetFrameHashScale(1);
    c2.OnFrame(vs);
    CHECK(!c2.LastFrame().grabbed && c2.LastFrame().hashed && c2.LastFrame().hash == 0, "no grab: hash 0, hashed");
}

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "mapping") TestMapping();
    if (which == "all" || which == "draws") TestDrawOrder();
    if (which == "all" || which == "frame") TestFrame();
    if (which == "all" || which == "framehash") TestFrameHash();
    std::printf("test_draw_ir_queue_consumer %s: %d checks, %d failures\n", which.c_str(), g_checks, g_fails);
    return g_fails ? 1 : 0;
}
