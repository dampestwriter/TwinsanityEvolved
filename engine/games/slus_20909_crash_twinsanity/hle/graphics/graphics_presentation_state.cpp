#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_presentation.h"

namespace title {
namespace {
bool Bad(std::string* error,const char* why) {
    if (error) *error = std::string("Graphics selected image: ") + why;
    return false;
}
}
bool GraphicsPresentation::CanCaptureState(std::string* error) const {
    if (!Active()) return true;
    PresentationState::Image metadata;
    metadata.kind = PresentationState::Kind::Scene; // extent validation without a readback
    metadata.frame = frame_;
    metadata.width = color_->description.width;
    metadata.height = color_->description.height;
    metadata.scale = color_->description.scale;
    return PresentationState::Validate(metadata,error);
}
bool GraphicsPresentation::CaptureState(PresentationState::Image& out,std::string* error) {
    if (!CanCaptureState(error)) return false;
    PresentationState::Image next;
    if (Active()) {
        ps2r::render::SurfaceId surface;
        if (resources_.Resolve(color_,&surface) != GraphicsResources::Result::Ok)
            return Bad(error,"selected allocation is unavailable");
        const auto& d = color_->description;
        next.frame = frame_; next.width = d.width; next.height = d.height; next.scale = d.scale;
        const auto* target = pipeline_.Cache().Find(frame_.color_buffer);
        const bool shared = target && target->views.color.id == surface.id;
        next.kind = shared ? PresentationState::Kind::Scene : PresentationState::Kind::Image;
        const uint64_t size = uint64_t(d.width)*d.height*4;
        if (size > PresentationState::kMaxBytes-132) return Bad(error,"image is too large");
        if (!shared) {
            next.rgba.resize(size);
            pipeline_.Backend().Readback(surface,{0,0,int32_t(d.width),int32_t(d.height)},
                                         next.rgba.data(),size_t(d.width)*4);
        }
    }
    if (!PresentationState::Validate(next,error)) return false;
    out = std::move(next);
    return true;
}
void GraphicsPresentation::DiscardStagedState() {
    staged_color_.reset();
    staged_frame_ = {};
    staged_valid_ = false;
    resources_.CollectAfterQuiescence();
}
bool GraphicsPresentation::StageState(const PresentationState::Image& image,std::string* error) {
    DiscardStagedState();
    if (!PresentationState::Validate(image,error)) return false;
    if (image.kind == PresentationState::Kind::None) { staged_valid_ = true; return true; }
    Graphics::Lease next;
    if (image.kind == PresentationState::Kind::Scene) {
        if (!scene_.RetainStagedColor(queue_,image.frame,next))
            return Bad(error,"shared image is absent from the staged scene");
        const auto& d = next->description;
        if (d.width != image.width || d.height != image.height || d.scale != image.scale)
            return Bad(error,"shared image dimensions differ");
    } else {
        if (image.width > uint32_t(pipeline_.Backend().QueryCaps().max_texture_size) ||
            image.height > uint32_t(pipeline_.Backend().QueryCaps().max_texture_size))
            return Bad(error,"image exceeds backend bounds");
        ps2r::render::SurfaceDesc d;
        d.width = image.width; d.height = image.height; d.scale = uint8_t(image.scale);
        d.renderable = true;
        Graphics::Handle handle;
        if (resources_.Register(queue_,d,&handle) != GraphicsResources::Result::Ok)
            return Bad(error,"image registration failed");
        const bool retained = queue_.RetainResource(handle,&next) == Graphics::Result::Ok;
        queue_.ReleaseResource(handle);
        if (!retained) return Bad(error,"image lease failed");
        Graphics::Upload upload;
        upload.resource = next;
        upload.rectangle = {0,0,int32_t(d.width),int32_t(d.height)};
        upload.stride = size_t(d.width)*4; upload.pixels = image.rgba;
        if (resources_.Upload(upload) != GraphicsResources::Result::Ok) {
            upload.resource.reset();
            next.reset();
            resources_.CollectAfterQuiescence();
            return Bad(error,"image staging upload failed");
        }
    }
    staged_color_ = std::move(next);
    staged_frame_ = image.frame;
    staged_valid_ = true;
    if (error) error->clear();
    return true;
}
bool GraphicsPresentation::PublishState(std::string* error) {
    if (!staged_valid_) return Bad(error,"no complete staged image");
    Clear();
    color_ = std::move(staged_color_);
    frame_ = staged_frame_;
    staged_frame_ = {};
    staged_valid_ = false;
    if (error) error->clear();
    return true;
}
} // namespace title
