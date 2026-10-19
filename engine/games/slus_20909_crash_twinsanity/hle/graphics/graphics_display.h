#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_intent.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_presentation.h"

namespace title {

// Source display conversion into an owned display image. The scene input is
// the shared actual attachment; the output is a named presentation resource.
// Resolve does not swap, clear, read back, create packets or touch a mirror.
class GraphicsDisplay {
public:
    GraphicsDisplay(GraphicsScene&, GraphicsResources&, GraphicsRenderer&,
                    ps2r::gs::DrawPipeline&, uint8_t scale);
    ~GraphicsDisplay();
    // Ok = the display image holds this frame. Refused = declined BEFORE any
    // draw (frame/plan mismatch, no valid source content, the scene not bound:
    // the ordered slot is released to the source's own copy). Failed = the
    // renderer or presentation failed after the copy began (a fault).
    enum class Resolution { Ok, Refused, Failed };
    Resolution Resolve(const FrameIntent::Output&, const GraphicsScene::Frame&);
    bool Matches(const GraphicsScene::Frame&) const;
    bool Active() const;
    bool Present(bool frame_end, ps2r::gs::DrawPipeline::Frame* capture);
    void Clear();
    // A live internal-scale change (the Display page): the display images are
    // released and re-allocated at the new factor by the next Resolve. On the
    // backend owner, between frames.
    void SetScale(uint8_t scale);
    uint8_t Scale() const { return scale_; }
    uint64_t Copies() const;
    bool CanCaptureState(std::string* error = nullptr) const;
    bool CaptureState(PresentationState::Image&,GraphicsScene::Frame&,std::string* error = nullptr);
    bool StageState(const PresentationState::Image&,const GraphicsScene::Frame&,std::string* error = nullptr);
    bool PublishState(std::string* error = nullptr);
    void DiscardStagedState();

private:
    bool Allocate(uint32_t width, uint32_t height);
    GraphicsScene& scene_;
    GraphicsResources& resources_;
    GraphicsRenderer& renderer_;
    ps2r::gs::DrawPipeline& pipeline_;
    Graphics queue_;
    GraphicsTargets targets_;
    GraphicsPresentation presentation_;
    std::array<Graphics::Handle,3> display_;
    GraphicsScene::Frame frame_;
    uint32_t width_ = 0, height_ = 0;
    uint8_t scale_ = 1;
    uint64_t copies_ = 0;
    GraphicsScene::Frame staged_frame_;
};
} // namespace title
