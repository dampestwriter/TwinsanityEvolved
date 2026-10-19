#include "games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.h"

#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "engine/ee/mem/main_memory.h"
#include "engine/ee/mem/scratchpad.h"
#include "engine/ee/savestate/memory_sections.h"
#include "engine/ee/savestate/state_restorer.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"
#include "tests/lib/fake_backend.h"

namespace {
using namespace ps2r;
using State = title::PresentationState;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(false)
class TestGraphicsPresentationState {
public:
    struct Backend : test::FakeBackend {
        int fail_create = -1;
        render::SurfaceId CreateSurface(const render::SurfaceDesc& desc) override {
            if (!fail_create) return {};
            if (fail_create > 0) --fail_create;
            return FakeBackend::CreateSurface(desc);
        }
    };
    title::GraphicsSurfaces surfaces;
    gs::VramImage vram;
    gs::VramLayout layout;
    gs::ScalePolicy scale;
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
    title::GraphicsScene scene{surfaces,resources,renderer,pipeline};
    title::GraphicsPresentation movie{scene,resources,pipeline};
    title::GraphicsDisplay display;
    MainMemory ram;
    Scratchpad scratch;
    MemorySections memory{ram,scratch,nullptr,0};
    StateRestorer restorer{memory};
    app::GraphicsCheckpoint checkpoint{scene,[](auto work) { return work(); },&movie,&display};
    title::FrameEnvironment::SceneFrame frame{0x70000,0x150000,512,448,7,31};
    unsigned pre_restore = 0;
    bool peer_ok = true;
    explicit TestGraphicsPresentationState(render::Backend& backend,int factor)
        : surfaces(backend),scale(gs::ScaleSettings{factor},&layout),
          display(scene,resources,renderer,pipeline,uint8_t(factor)) {
        pipeline.OnSetWindow(512,448,512,448);
        savestate::SectionSpec source;
        source.tag = title::FrameState::kTag; source.version = 1; source.owner = "source fixture";
        source.serialize = [this](auto& bytes) {
            title::FrameEnvironment::Snapshot snapshot;
            snapshot.source_epoch = frame.epoch;
            CHECK(title::FrameState::Encode(snapshot,bytes));
        };
        source.validate = [](const auto& blob,auto* error) {
            title::FrameEnvironment::Snapshot snapshot;
            return title::FrameState::Decode(blob.payload,snapshot,error);
        };
        source.restore = [](const auto&,auto*) { return true; };
        CHECK(restorer.Registry().Register(source));
        CHECK(checkpoint.Register(restorer));
        restorer.RequireSectionOwners();
        restorer.AddImagePreflight([this](const auto&,auto* error) {
            if (!peer_ok && error) *error = "peer refused";
            return peer_ok;
        });
        restorer.SetPreRestore([this](auto* error) {
            if (!checkpoint.CanCapture(error)) return false;
            ++pre_restore; return true;
        });
    }
    void Source(uint32_t rgba = 0xb687472f) {
        auto& target = cache.GetOrCreate(frame.color_buffer,512,448,0,0);
        surfaces.Clear(target.views.color,target.views.depth,rgba,0.375f);
        target.coherence.valid_rect.Union(0,0,512,448);
        target.coherence.has_content = true;
        target.identity.render_fbw = 512;
        target.identity.has_32bit_content = target.identity.was_32bit = true;
    }
    void Copy() {
        title::FrameIntent::Output intent;
        CHECK(title::FrameIntent::Build(512,448,{},intent) == title::FrameIntent::Result::Ok);
        CHECK(display.Resolve(intent,frame) == title::GraphicsDisplay::Resolution::Ok);
    }
    std::vector<uint8_t> Pixels(bool converted) {
        gs::DrawPipeline::Frame capture;
        CHECK(converted ? display.Present(false,&capture) : movie.Present(false,&capture));
        CHECK(capture.grabbed && capture.w == 512*cache.TargetScale(frame.color_buffer));
        return capture.rgba;
    }
    std::vector<uint8_t> StateBytes() {
        State::Snapshot snapshot;
        std::vector<uint8_t> bytes;
        CHECK(movie.CaptureState(snapshot.movie));
        CHECK(display.CaptureState(snapshot.display,snapshot.display_source));
        CHECK(State::Encode(snapshot,bytes));
        return bytes;
    }
};

void CodecBounds() {
    State::Snapshot snapshot;
    std::vector<uint8_t> bytes;
    CHECK(State::Encode(snapshot,bytes) && bytes.size() == 132);
    State::Snapshot decoded;
    CHECK(State::Decode(bytes,decoded));
    for (size_t length : {size_t(0),size_t(8),size_t(131)}) {
        std::vector<uint8_t> bad(bytes.begin(),bytes.begin()+length);
        CHECK(!State::Decode(bad,decoded));
    }
    bytes.push_back(0);
    CHECK(!State::Decode(bytes,decoded));
    snapshot.display_source.epoch = 1;
    CHECK(!State::Validate(snapshot));
    snapshot = {};
    snapshot.movie.kind = State::Kind::Image;
    snapshot.movie.frame = {0x70000,0x150000,64,32,7,31};
    snapshot.movie.width = 64; snapshot.movie.height = 32; snapshot.movie.scale = 1;
    snapshot.movie.rgba.resize(64*32*4,181);
    CHECK(State::Encode(snapshot,bytes));
    CHECK(State::Decode(bytes,decoded) && decoded.movie.rgba == snapshot.movie.rgba);
    snapshot.movie.scale = UINT32_MAX;
    CHECK(!State::Validate(snapshot));
    snapshot.movie.scale = 1; snapshot.movie.rgba.pop_back();
    CHECK(!State::Validate(snapshot));
    snapshot.movie.rgba.clear(); snapshot.movie.kind = State::Kind::Scene;
    CHECK(State::Validate(snapshot));
}

void DisplayRoundTrip(render::Backend& backend,int scale) {
    TestGraphicsPresentationState f(backend,scale);
    f.Source(); f.Copy();
    const auto expected = f.Pixels(true), presentation = f.StateBytes();
    const auto image = f.restorer.SaveImage({});
    const auto ticks = f.presenter.FrameCount(),game_ticks = f.presenter.EeFrameCount();
    f.Source(0x7f2143eb); f.Copy();
    f.ram.Data()[0x1000] = 91;
    const auto copies = f.display.Copies(),draws = f.pipeline.DrawSeq();
    f.pipeline.OnSetWindow(801,601,641,401);
    f.pipeline.OnSetFrontBuffer(0x2c0000,640,2);
    std::string error;
    f.peer_ok = false;
    CHECK(!f.restorer.RestoreImage(image.data(),image.size(),&error));
    CHECK(!f.pre_restore && f.ram.Data()[0x1000] == 91);
    CHECK(f.pipeline.WindowW() == 801 && f.pipeline.WindowH() == 601);
    f.peer_ok = true;
    CHECK(f.restorer.RestoreImage(image.data(),image.size(),&error));
    CHECK(f.pre_restore == 1 && f.ram.Data()[0x1000] == 0 && f.display.Matches(f.frame));
    CHECK(f.display.Copies() == copies && f.pipeline.DrawSeq() == draws);
    CHECK(f.pipeline.WindowW() == 512 && f.pipeline.WindowH() == 448 &&
          f.scanout.ContentW() == 512 && f.scanout.ContentH() == 448);
    CHECK(f.presenter.FrameCount() == ticks && f.presenter.EeFrameCount() == game_ticks);
    CHECK(f.StateBytes() == presentation && f.Pixels(true) == expected);
    CHECK(f.display.Copies() == copies && f.presenter.FrameCount() == ticks+1);
    f.Copy(); // restored held image transitions to the next real conversion
    CHECK(f.display.Copies() == copies+1 && f.Pixels(true) == expected);
    TestGraphicsPresentationState fresh(backend,scale);
    fresh.pipeline.OnSetWindow(127,93,71,65);
    CHECK(fresh.restorer.RestoreImage(image.data(),image.size(),&error));
    CHECK(fresh.display.Matches(fresh.frame) && fresh.display.Copies() == 0 &&
          fresh.pipeline.DrawSeq() == 0 && fresh.Pixels(true) == expected);
    CHECK(fresh.pipeline.WindowW() == 512 && fresh.pipeline.WindowH() == 448);
}

void SharedMovieIdentityAndOmission() {
    test::FakeBackend backend;
    TestGraphicsPresentationState f(backend,2);
    f.Source();
    CHECK(f.movie.Select(f.frame));
    State::Image selected;
    CHECK(f.movie.CaptureState(selected) && selected.kind == State::Kind::Scene && selected.rgba.empty());
    const auto image = f.restorer.SaveImage({});
    const auto original = f.cache.Find(f.frame.color_buffer)->views.color;
    f.Source(0xff123456);
    std::string error;
    CHECK(f.restorer.RestoreImage(image.data(),image.size(),&error));
    const auto restored = f.cache.Find(f.frame.color_buffer)->views.color;
    CHECK(original.id != restored.id && !backend.Has(original));
    f.Source(0xff876543);
    const auto expected = backend.At(restored).rgba;
    CHECK(f.Pixels(false) == expected && backend.last_present.source.id == restored.id);
    CHECK(backend.last_present.filter == render::PresentationFilter::Linear);
    CHECK(f.movie.Presents() == 1); // saved work adds no execution credit

    savestate::FileMeta meta;
    std::vector<savestate::Blob> sections;
    CHECK(savestate::ParseStateFile(image.data(),image.size(),&meta,&sections,&error));
    sections.erase(std::remove_if(sections.begin(),sections.end(),[](const auto& blob) {
        return blob.tag == State::kTag;
    }),sections.end());
    const auto absent = savestate::SerializeStateFile(meta,sections);
    f.ram.Data()[0x1000] = 91;
    CHECK(!f.restorer.RestoreImage(absent.data(),absent.size(),&error));
    CHECK(f.pre_restore == 1 && f.ram.Data()[0x1000] == 91 && f.Pixels(false) == expected);

    // A retired scene allocation still pinned by presentation is an owned
    // image at checkpoint; it must not alias the newer scene with the same key.
    std::vector<std::unique_ptr<gs::RenderTarget>> replacement;
    CHECK(f.cache.ReplaceAfterQuiescence(replacement,UINT32_MAX));
    f.Source(0x80fedcba);
    CHECK(f.movie.CaptureState(selected) && selected.kind == State::Kind::Image &&
          selected.rgba == expected);
    const auto detached = f.restorer.SaveImage({});
    CHECK(f.restorer.RestoreImage(detached.data(),detached.size(),&error));
    const auto new_scene = f.cache.Find(f.frame.color_buffer)->views.color;
    CHECK(f.Pixels(false) == expected && backend.last_present.source.id != new_scene.id);
    CHECK(backend.At(new_scene).rgba != expected);
}

void StagingFailureBeforeMutation() {
    TestGraphicsPresentationState::Backend backend;
    TestGraphicsPresentationState f(backend,2);
    f.Source(); f.Copy();
    const auto expected = f.Pixels(true);
    const auto image = f.restorer.SaveImage({});
    const int draws = backend.draws,clears = backend.clears;
    f.ram.Data()[0x1000] = 73;
    backend.fail_create = 3; // shared scene stages; selected image allocation fails
    std::string error;
    CHECK(!f.restorer.RestoreImage(image.data(),image.size(),&error));
    CHECK(!f.pre_restore && f.ram.Data()[0x1000] == 73 && f.Pixels(true) == expected);
    CHECK(backend.draws == draws && backend.clears == clears && f.display.Copies() == 1);
    backend.fail_create = -1;
    CHECK(f.restorer.RestoreImage(image.data(),image.size(),&error));
    CHECK(f.pre_restore == 1 && f.Pixels(true) == expected);

    CHECK(f.movie.Select(f.frame));
    const auto shared = f.restorer.SaveImage({});
    savestate::FileMeta meta;
    std::vector<savestate::Blob> sections;
    CHECK(savestate::ParseStateFile(shared.data(),shared.size(),&meta,&sections,&error));
    for (auto& blob : sections) {
        if (blob.tag != title::SceneState::kTag) continue;
        title::SceneState::Snapshot missing;
        CHECK(title::SceneState::Encode(missing,blob.payload)); // drops the selected shared color
    }
    const auto omitted = savestate::SerializeStateFile(meta,sections);
    f.ram.Data()[0x1000] = 91;
    CHECK(!f.restorer.RestoreImage(omitted.data(),omitted.size(),&error));
    CHECK(f.pre_restore == 1 && f.ram.Data()[0x1000] == 91 && f.display.Matches(f.frame));
    CHECK(backend.draws == draws && backend.clears == clears);
}

void CroppedMovieWindow(render::GlDevice& backend,std::vector<uint8_t>& shown) {
    TestGraphicsPresentationState f(backend,1);
    f.pipeline.OnSetWindow(64,64,512,448);
    auto& target = f.cache.GetOrCreate(f.frame.color_buffer,512,512,0,0);
    target.coherence.valid_rect.Union(0,0,512,512);
    target.coherence.has_content = true;
    target.identity.render_fbw = 512;
    target.identity.has_32bit_content = target.identity.was_32bit = true;
    std::vector<uint8_t> image(512*512*4);
    for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
        const auto at = (size_t(y)*512+x)*4;
        image[at] = y < 224 || y >= 448 ? 211 : 19;
        image[at+1] = 47;
        image[at+2] = y >= 224 ? 173 : 31;
        image[at+3] = 117;
    }
    backend.Upload(target.views.color,{0,0,512,512},image.data(),512*4);
    CHECK(f.movie.Select(f.frame));
    const auto check_window = [&] {
        unsigned differences = 0;
        CHECK(shown.size() == 64*64*4);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const int source_y = 447-((2*y+1)*448)/(2*64);
            const size_t from = (size_t(source_y)*512)*4,at = (size_t(y)*64+x)*4;
            for (unsigned c = 0; c < 3; ++c) differences += shown[at+c] != image[from+c];
            differences += shown[at+3] != 255; // displayed alpha is opaque, storage remains 117
        }
        CHECK(differences == 0);
    };
    CHECK(f.Pixels(false) == std::vector<uint8_t>(image.begin(),image.begin()+512*448*4));
    check_window();
    const auto saved_window = shown;
    const auto saved = f.restorer.SaveImage({});
    TestGraphicsPresentationState restored(backend,1);
    std::string error;
    CHECK(restored.restorer.RestoreImage(saved.data(),saved.size(),&error));
    CHECK(restored.Pixels(false) == std::vector<uint8_t>(image.begin(),image.begin()+512*448*4));
    CHECK(shown == saved_window);
    check_window();
    const auto presents = restored.movie.Presents();
    CHECK(restored.movie.Present(false,nullptr) && restored.movie.Presents() == presents+1);
    CHECK(shown == saved_window); // held tick retains the same crop after restore
}
}

int main(int argc,char** argv) {
    if (argc > 1 && std::strcmp(argv[1],"--gl") == 0) {
        platform::Window window;
        platform::GlContext context;
        if (!window.Create(true,1024,896,"Graphics presentation restore") || !context.Create(window)) return 77;
        render::GlDevice backend;
        std::vector<uint8_t> shown(64*64*4);
        CHECK(backend.Init([&] {
            glReadBuffer(GL_BACK);
            glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,shown.data());
            window.Swap();
        }));
        backend.SetWindowSize(1024,896);
        DisplayRoundTrip(backend,2);
        CroppedMovieWindow(backend,shown);
        CHECK(glGetError() == GL_NO_ERROR);
        backend.Shutdown();
    } else {
        CodecBounds();
        test::FakeBackend backend;
        DisplayRoundTrip(backend,1);
        SharedMovieIdentityAndOmission();
        StagingFailureBeforeMutation();
    }
    std::printf("GraphicsPresentationState: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
