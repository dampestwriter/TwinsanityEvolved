#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_presentation.h"

#include <stdexcept>

namespace title {
GraphicsPresentation::GraphicsPresentation(GraphicsScene& scene, GraphicsResources& resources,
                                           ps2r::gs::DrawPipeline& pipeline)
    : scene_(scene), resources_(resources), pipeline_(pipeline), targets_(queue_) {}

bool GraphicsPresentation::Select(const GraphicsScene::Frame& frame) {
    // Submission must have established the real scene. Priming is allowed
    // after its source clear; presentation must never create a blank twin.
    const auto* existing = pipeline_.Cache().Find(frame.color_buffer);
    if (!existing || !existing->coherence.HasValidContent()) return false;
    GraphicsScene::Binding next;
    if (scene_.Bind(queue_, targets_, frame, next) != GraphicsScene::Result::Ok) return false;
    Graphics::Lease color;
    if (queue_.RetainResource(next.handles[0], &color) != Graphics::Result::Ok) {
        GraphicsScene::Release(queue_, next);
        return false;
    }
    GraphicsScene::Release(queue_, binding_);
    binding_ = next;
    frame_ = frame;
    color_ = std::move(color);
    return true;
}

void GraphicsPresentation::Clear() {
    color_.reset();
    targets_.Reset();
    GraphicsScene::Release(queue_, binding_);
    frame_ = {};
}

bool GraphicsPresentation::SelectImage(Graphics::Lease color,const GraphicsScene::Frame& frame) {
    if (!color || !frame.width || !frame.height) return false;
    const auto& d = color->description;
    if (!d.renderable || d.format != ps2r::render::SurfaceFormat::Rgba8 || !d.scale ||
        uint64_t(frame.width)*d.scale > d.width || uint64_t(frame.height)*d.scale > d.height)
        return false;
    ps2r::render::SurfaceId surface;
    if (resources_.Resolve(color,&surface) != GraphicsResources::Result::Ok) return false;
    Clear();
    color_ = std::move(color);
    frame_ = frame;
    return true;
}

bool GraphicsPresentation::Present(bool ee_frame_end, ps2r::gs::DrawPipeline::Frame* capture) {
    if (!Active()) return false;
    ps2r::render::SurfaceId surface;
    if (resources_.Resolve(color_, &surface) != GraphicsResources::Result::Ok)
        throw std::runtime_error("Graphics presentation lost its selected scene");
    auto& backend = pipeline_.Backend();
    const auto description = backend.DescribeSurface(surface);
    const auto width = uint64_t(frame_.width) * description.scale;
    const auto height = uint64_t(frame_.height) * description.scale;
    if (!width || !height || width > description.width || height > description.height ||
        pipeline_.WindowW() <= 0 || pipeline_.WindowH() <= 0)
        throw std::runtime_error("Graphics presentation dimensions changed");
    const ps2r::render::Rect source{0, 0, int32_t(width), int32_t(height)};
    if (capture) {
        ps2r::gs::DrawPipeline::Frame next;
        next.w = int(width); next.h = int(height);
        next.rgba.resize(size_t(width * height * 4));
        backend.Readback(surface, source, next.rgba.data(), size_t(width * 4));
        next.grabbed = true;
        next.source = ps2r::gs::ScanoutComposer::Source::Target;
        next.source_fbp = frame_.color_buffer;
        next.source_fallback = false;
        *capture = std::move(next);
    }
    // OLD's window upscale is linear; image capture remains the exact stored
    // RGB/alpha bytes. This does not inherit or change a material's sampling.
    backend.Present({surface, source, {0, 0, pipeline_.WindowW(), pipeline_.WindowH()}, true,
                     ps2r::render::PresentationFilter::Linear});
    pipeline_.OnOwnedFrameEnd(ee_frame_end, frame_.color_buffer);
    ++presents_;
    return true;
}
}  // namespace title
