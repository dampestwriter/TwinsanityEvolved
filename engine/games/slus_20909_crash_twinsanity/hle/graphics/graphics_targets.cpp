#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_targets.h"

namespace title {
namespace {
using Format = ps2r::render::SurfaceFormat;
bool Extent(const ps2r::render::SurfaceDesc& a, const ps2r::render::SurfaceDesc& b) {
    return a.width == b.width && a.height == b.height && a.scale == b.scale;
}
}  // namespace

GraphicsTargets::GraphicsTargets(Graphics& queue) : queue_(queue) {}

GraphicsTargets::Result GraphicsTargets::Bind(Name name, const Graphics::Handle& handle) {
    const size_t index = static_cast<size_t>(name);
    if (index >= bindings_.size()) return Result::InvalidName;
    Graphics::Lease lease;
    if (queue_.RetainResource(handle, &lease) != Graphics::Result::Ok) return Result::InvalidBinding;
    const auto& desc = lease->description;
    if (name == Name::SceneDepth || name == Name::SkyDepth || name == Name::DisplayDepth) {
        if (desc.format != Format::Depth32f) return Result::InvalidBinding;
    } else {
        if (desc.format != Format::Rgba8) return Result::InvalidBinding;
        if (name != Name::SceneCopy && name != Name::SkyCopy &&
            name != Name::DisplayCopy && !desc.renderable) return Result::InvalidBinding;
    }
    bindings_[index] = {handle, std::move(lease)};
    return Result::Ok;
}

void GraphicsTargets::Unbind(Name name) {
    const size_t index = static_cast<size_t>(name);
    if (index < bindings_.size()) bindings_[index] = {};
}

void GraphicsTargets::Reset() { bindings_ = {}; }

GraphicsTargets::Result GraphicsTargets::Record(
    const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw, Name sampled) {
    if (draw.raster.frame.psm != 0 || draw.raster.zbuf.psm != 0)
        return Result::UnsupportedDraw;
    if (draw.tex_enabled != (sampled != Name::Count) ||
        (sampled != Name::Count && sampled != Name::SkyColor))
        return Result::UnsupportedDraw;
    std::vector<Graphics::Handle> handles;
    for (Name name : {Name::SceneColor, Name::SceneDepth, Name::SceneCopy, sampled}) {
        if (name == Name::Count) continue;
        const auto& binding = bindings_[static_cast<size_t>(name)];
        Graphics::Lease current;
        if (!binding.lease ||
            queue_.RetainResource(binding.handle, &current) != Graphics::Result::Ok ||
            current != binding.lease)
            return Result::MissingTarget;
        handles.push_back(binding.handle);
    }
    return RecordBindings(before, draw, handles);
}

GraphicsTargets::Result GraphicsTargets::RecordSky(
    const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw, const Graphics::Handle& texture) {
    if (!draw.tex_enabled || draw.use_fst || draw.tex.fst || draw.ctx || draw.tex.psm != 19 ||
        draw.raster.frame.psm || draw.raster.zbuf.psm ||
        !draw.raster.test.zte || draw.raster.test.ztst != 1)
        return Result::UnsupportedDraw;
    std::vector<Graphics::Handle> handles;
    for (Name name : {Name::SkyColor, Name::SkyDepth, Name::SkyCopy}) {
        const auto& binding = bindings_[static_cast<size_t>(name)];
        Graphics::Lease current;
        if (!binding.lease || queue_.RetainResource(binding.handle, &current) != Graphics::Result::Ok ||
            current != binding.lease) return Result::MissingTarget;
        handles.push_back(binding.handle);
    }
    Graphics::Lease image;
    if (queue_.RetainResource(texture, &image) != Graphics::Result::Ok ||
        image->description.renderable || image->description.format != Format::Rgba8 ||
        image->description.scale != 1) return Result::InvalidBinding;
    handles.push_back(texture);
    return RecordBindings(before, draw, handles);
}

GraphicsTargets::Result GraphicsTargets::RecordMovie(
    const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw, const Graphics::Handle& image) {
    // TR movie/player.cpp Draw, NTSC public MovieDraw 0x2aeb30. Picture UVs
    // already include source strip division, half texels and row crop.
    const auto& t = draw.tex;
    if (!draw.tex_enabled || !draw.use_fst || !t.fst || draw.ctx != 1 || draw.prim_type != 6 ||
        t.psm || t.tw != 10 || t.th != 10 || t.mmag || t.mmin || t.mxl || t.wms || t.wmt ||
        t.tcc != 1 || t.tfx != 1 || draw.raster.frame.psm || draw.raster.zbuf.psm)
        return Result::UnsupportedDraw;
    std::vector<Graphics::Handle> handles;
    for (auto name : {Name::SceneColor, Name::SceneDepth, Name::SceneCopy}) {
        const auto& binding = bindings_[static_cast<size_t>(name)];
        Graphics::Lease current;
        if (!binding.lease || queue_.RetainResource(binding.handle, &current) != Graphics::Result::Ok ||
            current != binding.lease) return Result::MissingTarget;
        handles.push_back(binding.handle);
    }
    Graphics::Lease picture;
    if (queue_.RetainResource(image, &picture) != Graphics::Result::Ok)
        return Result::InvalidBinding;
    const auto& d = picture->description;
    if (d.renderable || d.format != Format::Rgba8 || d.scale != 1 ||
        !d.width || !d.height || d.width > 4096 || d.height > 4096 || (d.width & 15) || (d.height & 15))
        return Result::InvalidBinding;
    handles.push_back(image);
    return RecordBindings(before, draw, handles);
}

GraphicsTargets::Result GraphicsTargets::RecordBindings(
    const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw,
    const std::vector<Graphics::Handle>& handles) {
    std::vector<Graphics::Lease> leases;
    for (const auto& handle : handles) {
        Graphics::Lease lease;
        if (queue_.RetainResource(handle, &lease) != Graphics::Result::Ok) return Result::MissingTarget;
        leases.push_back(std::move(lease));
    }
    const auto& color = leases[Color]->description;
    if (!Extent(color, leases[Depth]->description) || !Extent(color, leases[Copy]->description))
        return Result::InvalidBinding;
    // Sampling the target being written needs an explicit preceding copy pass,
    // never framebuffer feedback or a hidden texture snapshot.
    if (leases[Color]->lifetime == leases[Copy]->lifetime ||
        (draw.tex_enabled && (leases[Color]->lifetime == leases[Texture]->lifetime ||
                             leases[Copy]->lifetime == leases[Texture]->lifetime)))
        return Result::Feedback;
    return queue_.RecordDraw(before, draw, handles) == Graphics::Result::Ok
        ? Result::Ok : Result::QueueRejected;
}

GraphicsTargets::Result GraphicsTargets::RecordFrame(
    const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw) {
    if (draw.raster.frame.psm != 2 || draw.raster.zbuf.psm || !draw.raster.zbuf.zmsk ||
        !draw.tex_enabled || !draw.use_fst || !draw.tex.fst || draw.tex.psm != 1 ||
        draw.ctx != 1 || draw.prim_type != 6)
        return Result::UnsupportedDraw;
    std::vector<Graphics::Handle> handles;
    for (auto name : {Name::DisplayColor,Name::DisplayDepth,Name::DisplayCopy,Name::SceneColor}) {
        const auto& binding = bindings_[static_cast<size_t>(name)];
        Graphics::Lease current;
        if (!binding.lease || queue_.RetainResource(binding.handle,&current) != Graphics::Result::Ok ||
            current != binding.lease) return Result::MissingTarget;
        handles.push_back(binding.handle);
    }
    return RecordBindings(before,draw,handles);
}

GraphicsTargets::Result GraphicsTargets::RecordMesh(
    const Graphics::Anchor& before, const ps2r::gs::DrawIr& draw, const Graphics::Handle& texture) {
    if (!draw.tex_enabled || draw.use_fst || draw.tex.fst || draw.ctx ||
        (draw.tex.psm != 0 && draw.tex.psm != 19) || draw.raster.frame.psm || draw.raster.zbuf.psm)
        return Result::UnsupportedDraw;
    std::vector<Graphics::Handle> handles;
    for (auto name : {Name::SceneColor, Name::SceneDepth, Name::SceneCopy}) {
        const auto& binding = bindings_[static_cast<size_t>(name)];
        Graphics::Lease current;
        if (!binding.lease || queue_.RetainResource(binding.handle, &current) != Graphics::Result::Ok ||
            current != binding.lease) return Result::MissingTarget;
        handles.push_back(binding.handle);
    }
    Graphics::Lease image;
    if (queue_.RetainResource(texture, &image) != Graphics::Result::Ok ||
        image->description.renderable || image->description.format != Format::Rgba8 ||
        image->description.scale != 1) return Result::InvalidBinding;
    handles.push_back(texture);
    return RecordBindings(before, draw, handles);
}

}  // namespace title
