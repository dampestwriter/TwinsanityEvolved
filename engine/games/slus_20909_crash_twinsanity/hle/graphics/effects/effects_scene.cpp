#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/effects_scene.h"

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"

namespace title {

struct EffectsScene::Impl {
    GraphicsScene& scene;
    GraphicsResources& resources;
    ps2r::gs::DrawPipeline& pipeline;
    Graphics registry;
    GraphicsTargets targets{registry};
    GraphicsScene::Binding binding;
    ShadowRenderer shadows;
    WaveRenderer wave;
    DistortionRenderer distortion;
    ScreenRenderer screen;
    WaveGeometry::Grid grid; // the source MakeWaveGrid of the last screen pass height
    Counters counts;
    bool faulted = false;

    Impl(GraphicsScene& owner, GraphicsResources& images, ps2r::gs::DrawPipeline& retained)
        : scene(owner), resources(images), pipeline(retained), shadows(retained.Backend(), images),
          wave(retained.Backend(), images), distortion(retained.Backend(), images),
          screen(retained.Backend(), images) {}
    ~Impl() { Release(); }
    void Release() {
        targets.Reset();
        GraphicsScene::Release(registry, binding);
    }
    Result Reject(Result result) {
        Release();
        ++counts.rejected;
        return result;
    }
    Result Fail() {
        Release();
        ++counts.failures;
        faulted = true;
        return Result::DrawFailure;
    }
    ps2r::gs::DrawIr Publication(const Frame& frame, const ps2r::gs::DrawIr& draw) const {
        auto publication = draw;
        publication.raster.frame.fbp = frame.color_buffer;
        publication.raster.zbuf.zbp = frame.depth_buffer;
        return publication;
    }
    Result Acquire(const Frame& frame, const ps2r::gs::DrawIr& initial,
                   std::array<Graphics::Lease, 3>& images, ps2r::gs::DrawPipeline::Prepared& prepared) {
        if (scene.Bind(registry, targets, frame, binding) != GraphicsScene::Result::Ok)
            return Reject(Result::SceneUnavailable);
        auto* target = pipeline.Cache().Find(frame.color_buffer);
        if (!target) return Reject(Result::SceneUnavailable);
        const std::array<ps2r::render::SurfaceId, 3> expected{
            target->views.color, target->views.depth, target->views.rt_copy};
        for (size_t i = 0; i < images.size(); ++i) {
            ps2r::render::SurfaceId actual;
            if (registry.RetainResource(binding.handles[i], &images[i]) != Graphics::Result::Ok ||
                resources.Resolve(images[i], &actual) != GraphicsResources::Result::Ok ||
                actual.id != expected[i].id)
                return Reject(Result::ResourceFailure);
        }
        prepared.target = target;
        prepared.render_w = int(frame.width);
        prepared.render_h = int(frame.height);
        // This is the existing host scene clear protocol, not the retained
        // draw preparation path (which could resolve aliases and textures).
        target->PrepareDraw(pipeline.Backend(), Publication(frame, initial), pipeline.Present().EeFrameCount(),
                            pipeline.Scanout().ScanoutFbp(), &prepared.params);
        if (prepared.params.do_frame_clear)
            pipeline.Backend().Clear(expected[0], expected[1], 0, 0,
                ps2r::render::kClearDepth |
                (prepared.params.clear_color ? uint32_t{ps2r::render::kClearColor} : 0u));
        return Result::Ok;
    }
    auto Publisher(const Frame& frame, ps2r::gs::DrawPipeline::Prepared& prepared) {
        return [&, frame](const ps2r::gs::DrawIr& draw, const ps2r::render::DrawResult& result) {
            // Runs synchronously between Acquire and Release. CT32 publication
            // updates author/copy dirt/texture invalidation, never mirror bytes.
            pipeline.SetSrcFrame(frame.source_frame);
            pipeline.Finish(Publication(frame, draw), prepared, result);
            // The publication's place among the scene's draws (draw_routes.txt).
            pipeline.NotePublished(Publication(frame, draw), result);
            return true;
        };
    }
    Result Run(const Frame& frame, const std::vector<ps2r::gs::DrawIr>& volumes) {
        if (faulted) return Result::Faulted;
        // A pass with no accepted mesh publishes NOTHING: the source's empty
        // StartShadows/ApplyShadows strips change no scene pixel (the cleared
        // half-size buffer composites black), so no clear, copy or composite
        // is drawn and the scene clear protocol is not consumed here.
        if (volumes.empty()) {
            ++counts.empty_shadow_passes;
            return Result::Ok;
        }
        const ShadowDraw::Frame pass{frame.source_frame, frame.width, frame.height};
        std::vector<ShadowDraw::Command> commands;
        // Reject before acquiring images or consuming the scene clear.
        if (ShadowDraw::Build(pass, volumes, commands) != ShadowDraw::Result::Ok)
            return Reject(Result::InvalidInput);
        ShadowRenderer::Scene access;
        ps2r::gs::DrawPipeline::Prepared prepared;
        const auto acquired = Acquire(frame, commands.back().draw, access.images, prepared);
        if (acquired != Result::Ok) return acquired;
        access.publish = Publisher(frame, prepared);
        if (shadows.Draw(pass, volumes, access) != ShadowRenderer::Result::Ok) return Fail();
        Release();
        ++counts.shadow_passes;
        return Result::Ok;
    }
    Result RunWave(const Frame& frame, const WaveGeometry::Grid& grid, const WaveGeometry::Waves& waves) {
        if (faulted) return Result::Faulted;
        std::vector<ps2r::gs::DrawIr> draws;
        if (frame.width != 512 || grid.width != frame.width || grid.height != frame.height ||
            grid.height > 1024 || grid.strips.size() != 64 ||
            WaveGeometry::Build(grid, waves, frame.source_frame, draws) != WaveGeometry::Result::Ok ||
            draws.empty())
            return Reject(Result::InvalidInput);
        WaveRenderer::Scene access;
        ps2r::gs::DrawPipeline::Prepared prepared;
        const auto acquired = Acquire(frame, draws.front(), access.images, prepared);
        if (acquired != Result::Ok) return acquired;
        access.publish = Publisher(frame, prepared);
        if (wave.Draw(frame.source_frame, grid, waves, access) != WaveRenderer::Result::Ok) return Fail();
        Release();
        ++counts.wave_passes;
        return Result::Ok;
    }
    Result RunScreen(const Frame& frame, const ScreenFrame& pass) {
        if (faulted) return Result::Faulted;
        const auto& screen_pass = pass.screen;
        const bool draws_screen = screen_pass.blur || screen_pass.filter;
        if (frame.width != 512 || screen_pass.height != frame.height || !(draws_screen || pass.wave))
            return Reject(Result::InvalidInput);
        // The initial draw stands for the whole pass in the scene clear
        // protocol: blur composite, filter strips or the wave copy.
        std::vector<ps2r::gs::DrawIr> initial;
        if (screen_pass.blur) {
            if (ScreenDraw::Blur(frame.height, frame.source_frame, initial) != ScreenDraw::Result::Ok)
                return Reject(Result::InvalidInput);
            initial.erase(initial.begin(), initial.begin() + 2);
        } else {
            initial.emplace_back();
            if ((screen_pass.filter ? ScreenDraw::Filter(frame.height, frame.source_frame, false, initial.back())
                                    : ScreenDraw::WaveCopy(frame.height, frame.source_frame, initial.back())) !=
                ScreenDraw::Result::Ok)
                return Reject(Result::InvalidInput);
        }
        if (pass.wave && (grid.width != frame.width || grid.height != frame.height) &&
            WaveGeometry::MakeGrid(frame.width, frame.height, grid) != WaveGeometry::Result::Ok)
            return Reject(Result::InvalidInput);
        ScreenRenderer::Scene access;
        ps2r::gs::DrawPipeline::Prepared prepared;
        const auto acquired = Acquire(frame, initial.front(), access.images, prepared);
        if (acquired != Result::Ok) return acquired;
        access.publish = Publisher(frame, prepared);
        if (draws_screen && screen.Draw(frame.source_frame, screen_pass, access) != ScreenRenderer::Result::Ok)
            return Fail();
        if (pass.wave) {
            WaveRenderer::Scene wave_access{access.images, access.publish};
            if (wave.Draw(frame.source_frame, grid, pass.waves, wave_access) != WaveRenderer::Result::Ok)
                return Fail();
            ++counts.wave_passes;
        }
        Release();
        ++counts.screen_passes;
        return Result::Ok;
    }
    Result RunDistortion(const Frame& frame, const MaterialState::Environment& environment,
                         const std::vector<ps2r::gs::DrawIr>& draws) {
        if (faulted) return Result::Faulted;
        const DistortionRenderer::Frame pass{frame.source_frame, frame.width, frame.height};
        if (!DistortionRenderer::Valid(pass, draws)) return Reject(Result::InvalidInput);
        DistortionRenderer::Scene access;
        ps2r::gs::DrawPipeline::Prepared prepared;
        ps2r::gs::DrawIr initial;
        // Even an entirely culled block made its material's scene copy. The
        // source scene must therefore be prepared before that read.
        if (draws.empty()) {
            initial.raster.frame.psm = 0;
            initial.raster.zbuf.zmsk = true;
        } else initial = draws.front();
        const auto acquired = Acquire(frame, initial, access.images, prepared);
        if (acquired != Result::Ok) return acquired;
        access.publish = Publisher(frame, prepared);
        if (distortion.Draw(pass, environment, draws, access) != DistortionRenderer::Result::Ok) return Fail();
        Release();
        ++counts.distortion_passes;
        return Result::Ok;
    }
};

EffectsScene::EffectsScene(GraphicsScene& scene, GraphicsResources& resources,
                           ps2r::gs::DrawPipeline& pipeline)
    : impl_(new Impl(scene, resources, pipeline)) {}
EffectsScene::~EffectsScene() = default;
EffectsScene::Result EffectsScene::Shadows(const Frame& frame,
                                          const std::vector<ps2r::gs::DrawIr>& volumes) {
    try { return impl_->Run(frame, volumes); }
    catch (...) { return impl_->Fail(); }
}
EffectsScene::Result EffectsScene::Wave(const Frame& frame, const WaveGeometry::Grid& grid,
                                       const WaveGeometry::Waves& waves) {
    try { return impl_->RunWave(frame, grid, waves); }
    catch (...) { return impl_->Fail(); }
}
EffectsScene::Result EffectsScene::Distortion(const Frame& frame, const MaterialState::Environment& environment,
                                             const std::vector<ps2r::gs::DrawIr>& draws) {
    try { return impl_->RunDistortion(frame, environment, draws); }
    catch (...) { return impl_->Fail(); }
}
EffectsScene::Result EffectsScene::Screen(const Frame& frame, const ScreenFrame& pass) {
    try { return impl_->RunScreen(frame, pass); }
    catch (...) { return impl_->Fail(); }
}
void EffectsScene::ResetAfterQuiescence() {
    impl_->Release();
    impl_->shadows.ResetAfterQuiescence();
    impl_->wave.ResetAfterQuiescence();
    impl_->distortion.ResetAfterQuiescence();
    impl_->screen.ResetAfterQuiescence();
    impl_->faulted = false;
}
EffectsScene::Counters EffectsScene::Counts() const { return impl_->counts; }
ShadowRenderer::Counters EffectsScene::ShadowCounts() const { return impl_->shadows.Counts(); }
WaveRenderer::Counters EffectsScene::WaveCounts() const { return impl_->wave.Counts(); }
DistortionRenderer::Counters EffectsScene::DistortionCounts() const { return impl_->distortion.Counts(); }
ScreenRenderer::Counters EffectsScene::ScreenCounts() const { return impl_->screen.Counts(); }

}  // namespace title
