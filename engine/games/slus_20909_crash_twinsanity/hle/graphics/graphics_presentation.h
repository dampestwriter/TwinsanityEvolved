#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/presentation_state.h"

namespace title {

// Explicit scene presentation on the existing renderer frame owner. Selection
// occurs after source submission; each field reuses that exact allocation.
// No scanout address inference, local-memory mirror, extra clock or swap.
class GraphicsPresentation {
public:
    GraphicsPresentation(GraphicsScene&, GraphicsResources&, ps2r::gs::DrawPipeline&);
    bool Select(const GraphicsScene::Frame&);
    // Completed owned display conversion; retain the exact allocation. This
    // does not look up SceneColor or a model scanout address.
    bool SelectImage(Graphics::Lease, const GraphicsScene::Frame&);
    void Clear();
    bool Active() const { return bool(color_); }
    // false only while unselected. Selected failures throw, never fallback.
    bool Present(bool ee_frame_end, ps2r::gs::DrawPipeline::Frame* capture);
    uint64_t Presents() const { return presents_; }
    bool CanCaptureState(std::string* error = nullptr) const;
    bool CaptureState(PresentationState::Image&,std::string* error = nullptr);
    bool StageState(const PresentationState::Image&,std::string* error = nullptr);
    bool PublishState(std::string* error = nullptr);
    void DiscardStagedState();

private:
    GraphicsScene& scene_;
    GraphicsResources& resources_;
    ps2r::gs::DrawPipeline& pipeline_;
    Graphics queue_;
    GraphicsTargets targets_;
    GraphicsScene::Binding binding_;
    GraphicsScene::Frame frame_;
    Graphics::Lease color_;
    uint64_t presents_ = 0;
    Graphics::Lease staged_color_;
    GraphicsScene::Frame staged_frame_;
    bool staged_valid_ = false;
};

}  // namespace title
