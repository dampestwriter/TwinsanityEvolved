#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_display.h"

namespace title {
GraphicsDisplay::GraphicsDisplay(GraphicsScene& scene,GraphicsResources& resources,
    GraphicsRenderer& renderer,ps2r::gs::DrawPipeline& pipeline,uint8_t scale)
    : scene_(scene),resources_(resources),renderer_(renderer),pipeline_(pipeline),
      targets_(queue_),presentation_(scene,resources,pipeline),scale_(scale) {}

GraphicsDisplay::~GraphicsDisplay() { Clear(); }

bool GraphicsDisplay::Allocate(uint32_t width,uint32_t height) {
    if (width == width_ && height == height_) return true;
    if (!scale_ || scale_ > 4 || !width || !height || width > 1024 || height > 1024) return false;
    using Format = ps2r::render::SurfaceFormat;
    std::array<Graphics::Handle,3> next{};
    for (size_t i = 0; i < next.size(); ++i) {
        ps2r::render::SurfaceDesc description;
        description.width = width * scale_; description.height = height * scale_;
        description.scale = scale_;
        description.format = i == 1 ? Format::Depth32f : Format::Rgba8;
        description.renderable = i == 0;
        if (resources_.Register(queue_,description,&next[i]) != GraphicsResources::Result::Ok) {
            for (auto& handle : next) queue_.ReleaseResource(handle);
            return false;
        }
    }
    const std::array<GraphicsTargets::Name,3> names{
        GraphicsTargets::Name::DisplayColor,GraphicsTargets::Name::DisplayDepth,
        GraphicsTargets::Name::DisplayCopy};
    for (size_t i = 0; i < next.size(); ++i)
        if (targets_.Bind(names[i],next[i]) != GraphicsTargets::Result::Ok) {
            for (auto& handle : next) queue_.ReleaseResource(handle);
            return false;
        }
    for (auto& handle : display_) queue_.ReleaseResource(handle);
    display_ = next;
    width_ = width; height_ = height;
    return true;
}

GraphicsDisplay::Resolution GraphicsDisplay::Resolve(const FrameIntent::Output& intent,const GraphicsScene::Frame& frame) {
    if (intent.width != frame.width || intent.height != frame.height ||
        intent.draw.tex.tbp != frame.color_buffer || intent.draw.raster.zbuf.zbp != frame.depth_buffer ||
        intent.draw.scanmsk || !frame.epoch || !Allocate(intent.width,intent.height))
        return Resolution::Refused;
    const auto* source = pipeline_.Cache().Find(frame.color_buffer);
    if (!source || !source->coherence.HasValidContent()) return Resolution::Refused;
    GraphicsScene::Binding scene_binding;
    if (scene_.Bind(queue_,targets_,frame,scene_binding) != GraphicsScene::Result::Ok) return Resolution::Refused;
    bool ok = queue_.BeginFrame(frame.source_frame) == Graphics::Result::Ok;
    auto draw = intent.draw;
    draw.src_frame = frame.source_frame;
    if (ok) ok = targets_.RecordFrame(queue_.End(27),draw) == GraphicsTargets::Result::Ok;
    Graphics::Hooks hooks;
    hooks.draw = [&](const auto& command,const auto& bindings) {
        GraphicsRenderer::Frame controls;
        controls.width = intent.width; controls.height = intent.height; controls.display_copy = true;
        return renderer_.Draw(command,bindings,controls,nullptr) == GraphicsRenderer::Result::Ok;
    };
    hooks.drain = [] { return true; };
    if (ok) ok = queue_.Submit(hooks) == Graphics::Result::Ok;
    if (!ok && queue_.CurrentState() == Graphics::State::Faulted) queue_.Recover(hooks);
    GraphicsScene::Release(queue_,scene_binding);
    for (auto name : {GraphicsTargets::Name::SceneColor,GraphicsTargets::Name::SceneDepth,
                      GraphicsTargets::Name::SceneCopy}) targets_.Unbind(name);
    if (!ok) return Resolution::Failed;
    Graphics::Lease color;
    if (queue_.RetainResource(display_[0],&color) != Graphics::Result::Ok) return Resolution::Failed;
    auto shown = frame;
    shown.color_buffer = 0; // source display identity, never a backend lookup
    if (!presentation_.SelectImage(std::move(color),shown)) return Resolution::Failed;
    // The display consumed this exact generation, before any scene repaint.
    // Keep the existing mixed target clear protocol coherent.
    pipeline_.Cache().Find(frame.color_buffer)->MarkCompositeConsumed();
    frame_ = frame;
    ++copies_;
    return Resolution::Ok;
}

bool GraphicsDisplay::Matches(const GraphicsScene::Frame& f) const {
    return Active() && frame_.color_buffer == f.color_buffer && frame_.depth_buffer == f.depth_buffer &&
        frame_.width == f.width && frame_.height == f.height &&
        frame_.source_frame == f.source_frame && frame_.epoch == f.epoch;
}
bool GraphicsDisplay::Active() const { return presentation_.Active(); }
bool GraphicsDisplay::Present(bool frame_end,ps2r::gs::DrawPipeline::Frame* capture) {
    return presentation_.Present(frame_end,capture);
}
void GraphicsDisplay::Clear() {
    presentation_.Clear();
    targets_.Reset();
    for (auto& handle : display_) { queue_.ReleaseResource(handle); handle = {}; }
    frame_ = {};
    width_ = height_ = 0;
}
void GraphicsDisplay::SetScale(uint8_t scale) {
    if (!scale || scale > 4 || scale == scale_) return;
    Clear();
    scale_ = scale;
}
uint64_t GraphicsDisplay::Copies() const { return copies_; }
bool GraphicsDisplay::CanCaptureState(std::string* error) const {
    if (queue_.CurrentState() != Graphics::State::Idle) {
        if (error) *error = "Graphics display copy is not quiescent";
        return false;
    }
    return presentation_.CanCaptureState(error);
}
bool GraphicsDisplay::CaptureState(PresentationState::Image& image,GraphicsScene::Frame& source,
                                  std::string* error) {
    if (!CanCaptureState(error) || !presentation_.CaptureState(image,error)) return false;
    source = frame_;
    return true;
}
bool GraphicsDisplay::StageState(const PresentationState::Image& image,
    const GraphicsScene::Frame& source,std::string* error) {
    DiscardStagedState();
    PresentationState::Snapshot snapshot;
    snapshot.display = image; snapshot.display_source = source;
    if (!PresentationState::Validate(snapshot,error) || !CanCaptureState(error) ||
        (image.kind != PresentationState::Kind::None && image.scale != scale_) ||
        !presentation_.StageState(image,error)) return false;
    staged_frame_ = source;
    return true;
}
void GraphicsDisplay::DiscardStagedState() {
    presentation_.DiscardStagedState();
    staged_frame_ = {};
}
bool GraphicsDisplay::PublishState(std::string* error) {
    if (!CanCaptureState(error)) return false;
    // The saved image remains held through publication. New copies allocate
    // their own attachments lazily; saved execution earns no new copy credit.
    Clear();
    if (!presentation_.PublishState(error)) return false;
    frame_ = staged_frame_;
    staged_frame_ = {};
    return true;
}
} // namespace title
