#pragma once

#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/composition/graphics_checkpoint.h"
#include "engine/ee/savestate/state_restorer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"
#include "tests/lib/fake_backend.h"

// Real shared-scene/held-image owners over an in-memory backend. This joins
// Movie persistence, not raster or GPU-driver acceptance. FGRM uses an explicit
// unchanged source-frame fixture, as in GraphicsPresentationState.
class MovieCheckpointGraphics {
public:
    struct Backend : ps2r::test::FakeBackend {
        bool fail_create = false;
        unsigned create_attempts = 0;
        ps2r::render::SurfaceId CreateSurface(const ps2r::render::SurfaceDesc& desc) override {
            ++create_attempts;
            return fail_create ? ps2r::render::SurfaceId{} : FakeBackend::CreateSurface(desc);
        }
    } backend;
    title::GraphicsSurfaces surfaces{backend};
    ps2r::gs::VramImage vram;
    ps2r::gs::VramLayout layout;
    ps2r::gs::ScalePolicy scale{ps2r::gs::ScaleSettings{1}, &layout};
    ps2r::gs::RenderTargetCache cache{surfaces, vram, scale};
    ps2r::gs::TargetRefresher refresher{surfaces, vram, cache};
    ps2r::gs::TextureResolver resolver{surfaces, vram, cache, refresher, layout};
    ps2r::gs::ScanoutComposer scanout{surfaces, cache};
    ps2r::gs::Presenter presenter{scanout};
    ps2r::gs::SequenceRecipe sequence{ps2r::gs::SequenceRecipe::Constants{}};
    ps2r::gs::DrawPipeline pipeline{ps2r::gs::DrawPipeline::Deps{
        &surfaces, &vram, &cache, &refresher, &resolver, &scanout, &presenter, &layout, nullptr, &sequence}};
    title::GraphicsResources resources{surfaces};
    title::GraphicsRenderer renderer{surfaces, resources};
    title::GraphicsScene scene{surfaces, resources, renderer, pipeline};
    title::GraphicsPresentation movie{scene, resources, pipeline};
    title::GraphicsDisplay display{scene, resources, renderer, pipeline, 1};
    ps2r::app::GraphicsCheckpoint checkpoint{scene, [this](auto work) {
        ++ordered; return work();
    }, &movie, &display};
    title::FrameEnvironment::SceneFrame frame{0x70000, 0x150000, 64, 32, 7, 31};
    unsigned ordered = 0;

    bool Register(StateRestorer& restorer) {
        pipeline.OnSetWindow(64, 32, 64, 32);
        ps2r::savestate::SectionSpec source;
        source.tag = title::FrameState::kTag; source.version = 1; source.owner = "Movie source-frame fixture";
        source.serialize = [this](auto& bytes) {
            title::FrameEnvironment::Snapshot state;
            state.source_epoch = frame.epoch;
            if (!title::FrameState::Encode(state, bytes)) throw std::runtime_error("frame encoding failed");
        };
        source.validate = [](const auto& blob, auto* error) {
            title::FrameEnvironment::Snapshot state;
            return title::FrameState::Decode(blob.payload, state, error);
        };
        source.restore = [](const auto&, auto*) { return true; };
        return restorer.Registry().Register(source) && checkpoint.Register(restorer);
    }
    bool Populate(uint32_t color) {
        auto& target = cache.GetOrCreate(frame.color_buffer, 64, 32, 0, 0);
        surfaces.Clear(target.views.color, target.views.depth, color, 0.375f);
        target.coherence.valid_rect.Union(0, 0, 64, 32);
        target.coherence.has_content = true;
        target.identity.render_fbw = 64;
        target.identity.has_32bit_content = target.identity.was_32bit = true;
        return movie.Select(frame);
    }
    std::vector<uint8_t> Pixels() {
        ps2r::gs::DrawPipeline::Frame capture;
        if (!movie.Present(false, &capture) || !capture.grabbed || capture.w != 64 || capture.h != 32)
            throw std::runtime_error("held Movie image was not presented");
        return capture.rgba;
    }
};
