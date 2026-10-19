#pragma once

#include <map>

#include "engine/gs/hle/classrdr/draw_pipeline.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_renderer.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_surfaces.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_environment.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/scene_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_clear.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/display_layout.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_intent.h"

namespace title {

// Temporary composition boundary with the retained host DrawPipeline. It owns
// no hardware state or texture mirror. Source frame setup names the one scene;
// the typed path writes the retained pipeline's actual color/depth allocations.
class GraphicsScene {
public:
    using Frame = FrameEnvironment::SceneFrame;
    enum class Result { Ok, InvalidFrame, UnclosedDependency, ResourceFailure,
                        MissingScene, StaleScene, DrawFailure };
    struct Binding {
        std::array<Graphics::Handle, 3> handles;
    };
    GraphicsScene(GraphicsSurfaces& surfaces, GraphicsResources& resources,
                  GraphicsRenderer& renderer, ps2r::gs::DrawPipeline& retained);
    ~GraphicsScene();
    // Backend owner, with preceding retained work drained. A completed source
    // frame setup must precede selection. Old bindings are released explicitly;
    // recorded draws keep their versions. No extra clear or present here.
    Result Bind(Graphics& queue, GraphicsTargets& targets, const Frame&, Binding& out);
    // Source StartSkyBuffer names the retained half-size Sky allocation.
    // Reusing it preserves content across retained/selected/empty Sky passes.
    // No frame clear is introduced; only type-A ALWAYS depth is accepted.
    // An empty producer must preserve an existing authored image. Its caller
    // requests that explicitly before any scene allocation or composite.
    Result BindSky(Graphics& queue, GraphicsTargets& targets, const Frame&, Binding& out,
                   bool preserve = false);
    static void Release(Graphics& queue, Binding& binding);
    // Same ordered output slot as the queued draw. Detect resize/recreation and
    // unclosed alias/stencil dependencies before writing; never silently route
    // an already selected draw to a model.
    Result Draw(const ps2r::gs::DrawIr&, const std::vector<Graphics::Lease>&);
    static bool CanDrawStrip(const StripIntent::Output&, const Frame&);
    Result DrawStrip(const StripIntent::Output&, const Frame&);
    // Source bucket zero owns the clear rectangle/masks. This route suppresses
    // automatic target clearing and carries that decision into later mixed
    // draws, including checkpoint continuation. Requires preserved attachments
    // when the source does not fully overwrite them.
    Result StartFrame(const FrameClear::Output&, const Frame&);
    uint64_t FrameSetups() const;
    // Aliased/CPU-dirty pages under the colour buffer whose last VRAM author is
    // not this target itself: the target's own scanout writeback (the retained
    // present's deposit of the frame it drew) is not an unclosed dependency.
    bool ForeignPages(const Frame&, uint32_t footprint) const;
    Result ConfigureDisplay(const DisplayLayout::Output&);
    uint64_t DisplayLayouts() const;
    void CollectAfterQuiescence();
    // Backend-owner checkpoint operations, after all source/consumer work is
    // quiescent. Staging only allocates unpublished attachments; publish swaps
    // the complete shared target set without a mirror writeback or clear.
    bool CanCaptureState(std::string* error = nullptr) const;
    // The checkpoint boundary's settle, before CanCaptureState on the same
    // owner: every deferred retained writeback (the native present's "rows
    // owed to the pages") is deposited at its recorded epoch -- the same bytes
    // the first reader under it would have received -- so the captured image
    // owes the mirror nothing. Refuses by name while a shadow sequence is open.
    bool SettleForCapture(std::string* error = nullptr);
    uint64_t CaptureSettles() const { return capture_settles_; }
    uint64_t PreDrawSettles() const { return predraw_settles_; }
    SceneState::Viewport CaptureViewport() const;
    bool CaptureState(SceneState::Snapshot&, std::string* error = nullptr);
    bool StageState(const SceneState::Snapshot&, std::string* error = nullptr);
    bool PublishState(std::string* error = nullptr);
    void DiscardStagedState();
    // Preflight presentation references against the unpublished incoming scene.
    // No target lookup in the live/abandoned image and no blank allocation.
    bool RetainStagedColor(Graphics&,const Frame&,Graphics::Lease&);

private:
    struct Version {
        Frame frame;
        std::weak_ptr<const Graphics::Resource> color;
        std::array<ps2r::render::SurfaceId, 3> surfaces;
        bool sky = false;
    };
    Result BindTarget(Graphics&, GraphicsTargets&, const Frame&, Binding&, bool sky, bool preserve = false);
    Result DrawBound(const ps2r::gs::DrawIr&, const std::vector<Graphics::Lease>&, bool source_setup);
    bool Ready(const Frame&, const ps2r::gs::RenderTarget&) const;
    bool Capturable(const Frame&, const ps2r::gs::RenderTarget&) const;
    // The retained arm's own pre-draw rule (DrawPipeline::SameBufferPreDrawFlush):
    // a draw into a buffer whose deferred writeback still owes the mirror rows
    // deposits them first, before the content dies. Idle sequence only.
    void SettleBeforeDraw(const Frame&, const ps2r::gs::RenderTarget&);
    GraphicsSurfaces& surfaces_;
    GraphicsResources& resources_;
    GraphicsRenderer& renderer_;
    ps2r::gs::DrawPipeline& retained_;
    std::map<const Graphics::Resource*, Version> versions_;
    std::vector<std::unique_ptr<ps2r::gs::RenderTarget>> staged_;
    SceneState::Snapshot staged_history_;
    bool staged_valid_ = false;
    uint64_t frame_setups_ = 0;
    uint64_t display_layouts_ = 0;
    uint64_t capture_settles_ = 0, predraw_settles_ = 0;
};

}  // namespace title
