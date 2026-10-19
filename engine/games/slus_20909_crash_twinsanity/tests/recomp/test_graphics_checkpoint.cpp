#include "games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "tests/lib/fake_backend.h"

namespace {
using namespace ps2r;
using State = title::SceneState;
using Scene = title::GraphicsScene;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)

class TestGraphicsCheckpoint {
public:
    struct Backend : test::FakeBackend {
        int fail_create = -1;
        render::SurfaceId CreateSurface(const render::SurfaceDesc& d) override {
            if (fail_create == 0) return {};
            if (fail_create > 0) --fail_create;
            return FakeBackend::CreateSurface(d);
        }
    };
    Backend backend;
    title::GraphicsSurfaces surfaces{backend};
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
    MainMemory ram;
    Scratchpad scratch;
    MemorySections memory{ram,scratch,nullptr,0};
    StateRestorer restorer{memory};
    unsigned ordered_calls = 0, pre_restore = 0, resizes = 0;
    uint32_t resized_width = 0, resized_height = 0;
    bool legacy = false;   // the root's GraphicsImport "prepared" predicate
    app::GraphicsCheckpoint checkpoint{scene,[this](auto work) { ++ordered_calls; return work(); },
        nullptr,nullptr,[this](uint32_t width,uint32_t height) {
            CHECK(pipeline.WindowW() == int(width) && pipeline.WindowH() == int(height));
            resized_width = width; resized_height = height; ++resizes;
        }};
    Scene::Frame frame{0x70000,0x150000,64,32,7,31};
    explicit TestGraphicsCheckpoint(bool with_import = false) {
        if (with_import) checkpoint.SetLegacyImport([this] { return legacy; });
        CHECK(checkpoint.Register(restorer));
        restorer.RequireSectionOwners();
        restorer.SetPreRestore([this](std::string* why) {
            if (!checkpoint.CanCapture(why)) return false;
            ++pre_restore;
            return true;
        });
    }
    gs::DrawIr Draw() const {
        gs::DrawIr draw;
        draw.src_frame = frame.source_frame; draw.prim_type = 3;
        draw.raster.frame = {frame.color_buffer,frame.width,0,0};
        draw.raster.zbuf = {frame.depth_buffer,0,false};
        draw.raster.test.zte = true; draw.raster.test.ztst = 2;
        draw.raster.scissor = {0,0,uint16_t(frame.width-1),uint16_t(frame.height-1)};
        draw.verts.resize(3); draw.vertex_count = 3;
        draw.verts[1].x = 16; draw.verts[2].y = 16;
        return draw;
    }
    void Populate() {
        pipeline.SetGfxEpoch(91);
        presenter.RestoreTimeline(11,7);
        CHECK(pipeline.DrawGeneric(Draw()).drew);
        auto& t = *cache.Find(frame.color_buffer);
        // Asymmetric alpha/color and nonconstant physical depth catch a
        // logical-size-only readback or a missing alpha channel (OLD SH7/SH8).
        const auto pixels = size_t(t.identity.PhysicalW()) * t.identity.PhysicalH();
        std::vector<uint8_t> rgba(pixels * 4), copy(pixels * 4);
        std::vector<float> depth(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            depth[i] = float(i % 1024) / 1024.0f;
            for (unsigned c = 0; c < 4; ++c) {
                rgba[i * 4 + c] = uint8_t(i * (c + 1) + c * 31);
                copy[i * 4 + c] = uint8_t(255 - rgba[i * 4 + c]);
            }
        }
        const render::Rect r{0,0,t.identity.PhysicalW(),t.identity.PhysicalH()};
        surfaces.Upload(t.views.color,r,rgba.data(),size_t(r.w) * 4);
        surfaces.Upload(t.views.depth,r,depth.data(),size_t(r.w) * 4);
        surfaces.Upload(t.views.rt_copy,r,copy.data(),size_t(r.w) * 4);
        t.coherence.cd_copy.valid = true;
        t.coherence.cd_copy.last_route = 0;
    }
    std::vector<uint8_t> Capture() {
        State::Snapshot snapshot;
        std::vector<uint8_t> bytes;
        std::string error;
        CHECK(scene.CaptureState(snapshot,&error));
        CHECK(State::Encode(snapshot,bytes,&error));
        return bytes;
    }
};

void CodecBounds() {
    TestGraphicsCheckpoint f;
    f.Populate();
    const auto bytes = f.Capture();
    State::Snapshot decoded;
    CHECK(State::Decode(bytes,decoded));
    std::vector<uint8_t> encoded;
    CHECK(State::Encode(decoded,encoded) && encoded == bytes);
    for (size_t length : {size_t(0),size_t(39),size_t(67),size_t(69),bytes.size()-1}) {
        std::vector<uint8_t> bad(bytes.begin(),bytes.begin()+length);
        CHECK(!State::Decode(bad,decoded));
    }
    auto bad = bytes;
    bad.push_back(0);
    CHECK(!State::Decode(bad,decoded));
    CHECK(State::Decode(bytes,decoded));
    auto saved = decoded;
    decoded.targets[0].identity.scale = 255;
    CHECK(!State::Validate(decoded));
    decoded = saved; decoded.targets.push_back(decoded.targets[0]);
    CHECK(!State::Validate(decoded));
    decoded = saved; decoded.targets[0].depth[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!State::Validate(decoded));
    decoded = saved; decoded.targets[0].history.last_z_seq = decoded.order + 1;
    CHECK(!State::Validate(decoded));
    decoded = saved; decoded.targets[0].copy.pop_back();
    CHECK(!State::Validate(decoded));
    decoded = saved; decoded.targets.clear();
    CHECK(!State::Validate(decoded));
    for (unsigned change = 0; change < 7; ++change) {
        decoded = saved;
        auto& view = decoded.viewport;
        if (change == 0) view.width = 0;
        if (change == 1) view.height = UINT32_MAX;
        if (change == 2) view.content_width = view.content_height = 2049;
        if (change == 3) { view.content_width = 1; view.content_height = 0; }
        if (change == 4) view.color_key = 7;
        if (change == 5) view.buffer_width = 63;
        if (change == 6) view.format = 32;
        CHECK(!State::Validate(decoded));
    }
    auto old = bytes; old[4] = 1; // no synthesis of viewport state from GSCN v1
    CHECK(!State::Decode(old,decoded));
    // v3: a 16-bit frame target (the title screen's PSMCT16 display buffers)
    // keeps its format, 16-bit history and keep mask through the codec; the
    // alt16 companion's authorship still refuses (a second attachment).
    decoded = saved;
    auto& sixteen = decoded.targets[0];
    sixteen.identity.render_psm = 2; sixteen.identity.was_16bit = true;
    sixteen.history.sync_keep_mask16 = 0x8000;
    CHECK(State::Validate(decoded));
    std::vector<uint8_t> v3;
    CHECK(State::Encode(decoded,v3));
    State::Snapshot back;
    CHECK(State::Decode(v3,back) && back.targets[0].identity.render_psm == 2 &&
          back.targets[0].identity.was_16bit && back.targets[0].history.sync_keep_mask16 == 0x8000);
    decoded = saved; decoded.targets[0].identity.render_psm = 0x13;   // a texture format is no frame
    CHECK(!State::Validate(decoded));
    decoded = saved; decoded.targets[0].history.alt_author_seq = 1;
    CHECK(!State::Validate(decoded));
    // A v2 image (32-bit targets only, no format/keep words) still decodes.
    std::vector<uint8_t> v2(bytes.begin(), bytes.begin() + 68);
    v2[4] = 2;
    size_t at = 68;
    for (size_t i = 0; i < saved.targets.size(); ++i) {
        const auto& t = saved.targets[i];
        const size_t pixels = size_t(t.identity.PhysicalW()) * t.identity.PhysicalH();
        v2.insert(v2.end(), bytes.begin() + at, bytes.begin() + at + 24);   // fbp..scale, flags
        at += 24 + 8;                                                       // skip render_psm, keep16
        const size_t rest = 16 * 4 + 4 + 8 + 4 + 4 + 4 + 8 + 4 + 8 + 8 + 4 + 8 + 4 + 5 * 4 +
            pixels * 8 + (t.history.cd_copy.valid ? pixels * 4 : 0);
        v2.insert(v2.end(), bytes.begin() + at, bytes.begin() + at + rest);
        at += rest;
    }
    CHECK(at == bytes.size());
    State::Snapshot from_v2;
    CHECK(State::Decode(v2,from_v2) && from_v2.targets.size() == saved.targets.size() &&
          from_v2.targets[0].identity.render_psm == 0 && !from_v2.targets[0].identity.was_16bit);
    std::vector<uint8_t> again;
    CHECK(State::Encode(from_v2,again) && again == bytes);   // re-encoded as v3, same content
}

void SurfaceRoundTripAndContinuation() {
    for (bool consumed : {false,true}) {
        TestGraphicsCheckpoint f;
        f.Populate();
        auto* live = f.cache.Find(f.frame.color_buffer);
        if (consumed) live->MarkCompositeConsumed();
        const bool expected_clear = live->WantsFrameClear(f.presenter.EeFrameCount());
        const auto original_color = live->views.color;
        auto pin = f.surfaces.Pin(original_color);
        const auto before = f.Capture();
        const auto state = f.restorer.SaveImage({});
        const auto width = f.pipeline.WindowW(),height = f.pipeline.WindowH();
        const auto content_w = f.scanout.ContentW(),content_h = f.scanout.ContentH();
        const auto front = f.scanout.ScanoutFbp(),pitch = f.scanout.DispfbFbw(),format = f.scanout.DispfbPsm();
        const std::vector<uint8_t> vram(f.vram.Bytes(),f.vram.Bytes()+gs::VramImage::kBytes);
        const auto epoch = f.vram.Page(f.frame.color_buffer / 8192).upload_epoch;
        f.pipeline.DrawGeneric(f.Draw()); // abandoned work must stay in the execution census
        f.presenter.MarkEeFrameEnd();
        f.surfaces.Clear(live->views.color,live->views.depth,0xffffffff,1);
        f.cache.GetOrCreate(0x180000,64,32,0,0); // only in the abandoned timeline
        f.pipeline.OnSetWindow(801,601,641,401);
        f.pipeline.OnSetFrontBuffer(0x200000,640,2);
        const auto draws = f.pipeline.DrawSeq();
        const auto clears = f.backend.clears, presents = f.backend.presents, reads = f.backend.readbacks;
        std::string error;
        CHECK(f.restorer.RestoreImage(state.data(),state.size(),&error));
        CHECK(f.pre_restore == 1 && f.cache.Count() == 1 && !f.cache.Find(0x180000));
        CHECK(f.backend.clears == clears && f.backend.presents == presents);
        CHECK(f.backend.readbacks == reads); // no discarded-target rescue
        CHECK(std::memcmp(vram.data(),f.vram.Bytes(),vram.size()) == 0);
        CHECK(f.vram.Page(f.frame.color_buffer / 8192).upload_epoch == epoch);
        CHECK(f.pipeline.DrawSeq() == draws && f.pipeline.DrawOrder() == 1);
        CHECK(f.pipeline.WindowW() == width && f.pipeline.WindowH() == height &&
              f.scanout.ContentW() == content_w && f.scanout.ContentH() == content_h);
        CHECK(f.scanout.ScanoutFbp() == front && f.scanout.DispfbFbw() == pitch &&
              f.scanout.DispfbPsm() == format);
        CHECK(f.resizes == 1 && f.resized_width == uint32_t(width) && f.resized_height == uint32_t(height));
        CHECK(f.Capture() == before);
        live = f.cache.Find(f.frame.color_buffer);
        CHECK(live->views.color.id != original_color.id && f.backend.Has(original_color));
        CHECK(live->WantsFrameClear(f.presenter.EeFrameCount()) == expected_clear);
        CHECK(f.pipeline.DrawGeneric(f.Draw()).drew);
        CHECK(f.backend.last_params.do_frame_clear == expected_clear);
        CHECK(f.pipeline.DrawSeq() == draws + 1 && live->coherence.last_z_seq == 2);
        CHECK(f.backend.last_params.color.id == live->views.color.id);
        pin.reset();
        f.scene.CollectAfterQuiescence();
        CHECK(!f.backend.Has(original_color));
        CHECK(f.restorer.RestoreImage(state.data(),state.size(),&error));
        CHECK(f.resizes == 1); // identical source dimensions do not resize the GUI again
    }
}

void EmptyViewportAndRequiredVersion() {
    TestGraphicsCheckpoint f;
    f.scanout.SetScanoutFbp(UINT32_MAX); // actual VideoStack cold-start sentinel
    const auto cold = f.restorer.SaveImage({});
    f.Populate();
    f.pipeline.OnSetWindow(1024,768,640,512);
    f.pipeline.OnSetFrontBuffer(0x1c0000,640,2);
    CHECK(f.restorer.RestoreImage(cold.data(),cold.size(),nullptr));
    CHECK(f.cache.Count() == 0 && f.pipeline.WindowW() == 640 && f.pipeline.WindowH() == 448);
    CHECK(f.scanout.ContentW() == 0 && f.scanout.ContentH() == 0 &&
          f.scanout.ScanoutFbp() == UINT32_MAX && !f.scanout.DispfbFbw());
    CHECK(f.resizes == 1 && !f.backend.presents);
    savestate::FileMeta metadata;
    std::vector<savestate::Blob> sections;
    CHECK(savestate::ParseStateFile(cold.data(),cold.size(),&metadata,&sections,nullptr));
    for (auto& section : sections) if (section.tag == State::kTag) {
        section.version = 1;
        section.payload[4] = 1;
    }
    const auto old = savestate::SerializeStateFile(metadata,sections);
    f.ram.Data()[0x1000] = 73;
    CHECK(!f.restorer.RestoreImage(old.data(),old.size(),nullptr));
    CHECK(f.ram.Data()[0x1000] == 73 && f.pre_restore == 1 && f.resizes == 1);
}

void RejectionBeforePublication() {
    TestGraphicsCheckpoint f;
    f.Populate();
    const auto before = f.Capture();
    const auto state = f.restorer.SaveImage({});
    const auto original = f.cache.Find(f.frame.color_buffer)->views.color;
    for (int fail = 0; fail < 3; ++fail) {
        f.backend.fail_create = fail;
        std::string error;
        CHECK(!f.restorer.RestoreImage(state.data(),state.size(),&error));
        CHECK(!f.pre_restore && !f.resizes &&
              f.cache.Find(f.frame.color_buffer)->views.color.id == original.id);
        CHECK(f.Capture() == before);
    }
    f.backend.fail_create = -1;
    bool peer_ok = false;
    f.restorer.AddImagePreflight([&](const auto&, auto* error) {
        if (!peer_ok && error) *error = "peer refused";
        return peer_ok;
    });
    std::string error;
    CHECK(!f.restorer.RestoreImage(state.data(),state.size(),&error));
    CHECK(!f.pre_restore && f.Capture() == before);
    peer_ok = true;
    CHECK(f.restorer.RestoreImage(state.data(),state.size(),&error));
    CHECK(f.pre_restore == 1 && f.Capture() == before);
    State::Snapshot incompatible;
    CHECK(State::Decode(before,incompatible));
    incompatible.targets[0].identity.scale = 1;
    auto& target = incompatible.targets[0];
    target.color.resize(64 * 32 * 4); target.copy.resize(64 * 32 * 4); target.depth.resize(64 * 32);
    CHECK(!f.scene.StageState(incompatible,&error));
    CHECK(f.Capture() == before);
    f.cache.AliasPages().MarkAliased(f.frame.color_buffer,8192);
    CHECK(!f.checkpoint.CanCapture(&error));
    f.cache.AliasPages().ClearAliased(f.frame.color_buffer,8192);
    // A deferred retained writeback is SETTLED at the checkpoint boundary
    // (GraphicsScene::SettleForCapture: deposited at its recorded epoch, the
    // ledger drained, Reason::Save counted) -- the captured image owes the
    // mirror nothing; it is no longer a refusal.
    f.cache.Ledger().Record(f.frame.color_buffer,8192,1);
    const auto settles = f.scene.CaptureSettles();
    CHECK(f.checkpoint.CanCapture(&error));
    CHECK(f.scene.CaptureSettles() == settles + 1 && f.cache.Ledger().Empty() &&
          f.cache.Ledger().Flushes(gs::PendingRtSyncLedger::Reason::Save) == 1);
    f.sequence.Prime(0x230000);
    CHECK(!f.checkpoint.CanCapture(&error));
}

void EmptyIsOwnedState() {
    TestGraphicsCheckpoint f;
    const auto state = f.restorer.SaveImage({});
    f.Populate();
    const auto draws = f.pipeline.DrawSeq();
    std::string error;
    CHECK(f.restorer.RestoreImage(state.data(),state.size(),&error));
    CHECK(f.cache.Count() == 0 && f.pipeline.DrawSeq() == draws && f.pipeline.DrawOrder() == 0);
    MemorySections memory(f.ram,f.scratch,nullptr,0);
    StateRestorer missing(memory);
    const auto no_scene = missing.SaveImage({});
    CHECK(!f.restorer.RestoreImage(no_scene.data(),no_scene.size(),&error));
    CHECK(f.cache.Count() == 0 && f.pre_restore == 1);
}

// The explicit legacy import: a frozen image (no GSCN) is admitted ONLY while
// the root's import holds a prepared image; its absence publishes nothing
// owned -- the retained pipeline's own VRAM restore governs the live targets
// (the retained arm's path for the same anchors). Without the import, or
// with it unprepared, the section stays required.
void LegacyImportAbsentSection() {
    TestGraphicsCheckpoint f(true);
    MemorySections memory(f.ram,f.scratch,nullptr,0);
    StateRestorer frozen(memory);
    const auto legacy = frozen.SaveImage({});   // RAM0/SPAD only: the OLD schema
    f.Populate();
    const auto draws = f.pipeline.DrawSeq();
    std::string error;
    CHECK(!f.restorer.RestoreImage(legacy.data(),legacy.size(),&error));   // import not prepared
    CHECK(error.find("Graphics shared surfaces") != std::string::npos && f.pre_restore == 0);
    f.legacy = true;
    CHECK(f.restorer.RestoreImage(legacy.data(),legacy.size(),&error));
    CHECK(f.pre_restore == 1 && f.cache.Count() == 1 && f.pipeline.DrawSeq() == draws);   // live targets kept
    CHECK(f.checkpoint.CanCapture(&error));
    const auto captured = f.restorer.SaveImage({});   // the owner captures GSCN again (capture_required)
    f.legacy = false;
    CHECK(f.restorer.RestoreImage(captured.data(),captured.size(),&error) && f.pre_restore == 2);
    TestGraphicsCheckpoint plain;
    CHECK(!plain.restorer.RestoreImage(legacy.data(),legacy.size(),&error) && plain.pre_restore == 0);
}

void MultipleTargetsInFreshOwner() {
    TestGraphicsCheckpoint source;
    source.Populate();
    auto second = source.Draw();
    second.raster.frame.fbp = 0x230000;
    CHECK(source.pipeline.DrawGeneric(second).drew);
    State::Snapshot saved;
    CHECK(source.scene.CaptureState(saved));
    CHECK(saved.targets.size() == 2 && saved.order == 2);
    TestGraphicsCheckpoint fresh;
    CHECK(fresh.scene.StageState(saved));
    CHECK(fresh.scene.PublishState());
    CHECK(fresh.pipeline.DrawSeq() == 0 && fresh.pipeline.DrawOrder() == 2);
    CHECK(fresh.cache.Count() == 2 && fresh.cache.MostRecentDrawnFbp() == 0x230000);
    std::vector<uint8_t> expected;
    CHECK(State::Encode(saved,expected));
    CHECK(fresh.Capture() == expected);
    CHECK(fresh.cache.Find(0x70000)->coherence.last_z_seq == 1);
    CHECK(fresh.cache.Find(0x230000)->coherence.last_z_seq == 2);
    CHECK(fresh.pipeline.DrawGeneric(fresh.Draw()).drew);
    CHECK(!fresh.backend.last_params.do_frame_clear);
    CHECK(fresh.pipeline.DrawSeq() == 1 && fresh.cache.Find(0x70000)->coherence.last_z_seq == 3);
}
}

int main() {
    CodecBounds();
    SurfaceRoundTripAndContinuation();
    RejectionBeforePublication();
    EmptyIsOwnedState();
    EmptyViewportAndRequiredVersion();
    LegacyImportAbsentSection();
    MultipleTargetsInFreshOwner();
    std::printf("GraphicsCheckpoint: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
