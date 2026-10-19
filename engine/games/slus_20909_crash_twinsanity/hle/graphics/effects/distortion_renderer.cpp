#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/distortion_renderer.h"

#include <cmath>

#include "engine/gs/hle/texsrc/texture_source.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"

namespace title {
namespace {
using namespace ps2r::render;
ps2r::gs::TextureSource Texture(SurfaceId image, const SurfaceDesc& description,
                                const ps2r::gs::DrawIr& draw) {
    ps2r::gs::TextureSource texture;
    texture.enabled = true;
    texture.owner.kind = ps2r::gs::TextureSource::OwnerKind::Surface;
    texture.owner.surface = image;
    texture.transform.uv_tex_w = int(description.width / description.scale);
    texture.transform.uv_tex_h = int(description.height / description.scale);
    texture.transform.uv_pre_w = 1 << draw.tex.tw;
    texture.transform.uv_pre_h = 1 << draw.tex.th;
    texture.transform.draw_scale = description.scale;
    texture.sampler.mag_linear = draw.tex.mmag;
    texture.sampler.mmin = draw.tex.mmin;
    texture.sampler.wms = draw.tex.wms; texture.sampler.wmt = draw.tex.wmt;
    return texture;
}
}
struct DistortionRenderer::Impl {
    Backend& backend;
    GraphicsResources& resources;
    Graphics registry;
    std::array<Graphics::Handle, 3> handles{};
    std::array<Graphics::Lease, 3> leases{};
    std::array<SurfaceId, 3> images{};
    uint8_t scale = 0;
    uint32_t backing_height = 0;
    Counters counts;
    bool faulted = false;
    Impl(Backend& device, GraphicsResources& owner) : backend(device), resources(owner) {}
    void Release() {
        for (auto& handle : handles) { registry.ReleaseResource(handle); handle = {}; }
        leases = {}; images = {}; scale = 0; backing_height = 0;
    }
    Result Fail(Result result) { faulted = true; ++counts.failures; return result; }
    bool Allocate(uint8_t requested_scale, uint32_t requested_height) {
        if (scale == requested_scale && backing_height == requested_height) return true;
        Release();
        for (size_t i = 0; i < handles.size(); ++i) {
            const SurfaceDesc description{uint32_t(256 * requested_scale), requested_height * requested_scale,
                i == 1 ? SurfaceFormat::Depth32f : SurfaceFormat::Rgba8, true, requested_scale};
            if (resources.Register(registry, description, &handles[i]) != GraphicsResources::Result::Ok ||
                registry.RetainResource(handles[i], &leases[i]) != Graphics::Result::Ok ||
                resources.Resolve(leases[i], &images[i]) != GraphicsResources::Result::Ok) return false;
            ++counts.allocations;
        }
        scale = requested_scale;
        backing_height = requested_height;
        return true;
    }
    bool SceneImages(const Frame& frame, const Scene& scene, std::array<SurfaceId, 3>& output) {
        if (!scene.publish) return false;
        for (const auto& image : scene.images) if (!image || !image->lifetime) return false;
        const auto& color = scene.images[0]->description;
        if (!color.scale || !color.renderable || color.width % color.scale || color.height % color.scale ||
            color.width < frame.width * color.scale || color.height < frame.height * color.scale) return false;
        for (size_t i = 0; i < output.size(); ++i) {
            const auto& description = scene.images[i]->description;
            if (description.width != color.width || description.height != color.height ||
                description.scale != color.scale ||
                description.format != (i == 1 ? SurfaceFormat::Depth32f : SurfaceFormat::Rgba8) ||
                resources.Resolve(scene.images[i], &output[i]) != GraphicsResources::Result::Ok) return false;
            for (size_t j = 0; j < i; ++j) if (output[i].id == output[j].id) return false;
        }
        return true;
    }
    Result Execute(const Frame& frame, const ps2r::gs::DrawIr& copy,
                   const std::vector<ps2r::gs::DrawIr>& draws, const Scene& scene,
                   const std::array<SurfaceId, 3>& scene_images) {
        DrawParams parameters;
        parameters.color = images[0]; parameters.depth = images[1]; parameters.rt_copy = images[2];
        parameters.rt_copy_available = true;
        parameters.render_w = parameters.target_w = 256;
        // Keep the shared renderer's screen-height backing extent. The
        // authored copy remains a 256-square rectangle; changing only the
        // backing extent changes linear-sampling rounding at the interface.
        parameters.render_h = parameters.target_h = int(backing_height);
        parameters.scale = scale; parameters.has_32bit_content = true;
        // Private CT24 RGB is fully authored by the copy. Its alpha is never
        // sampled by type18; define it rather than exposing pooled contents.
        backend.Clear(images[0], images[1], 0, 0, kClearColor | kClearDepth);
        if (!backend.Draw(copy, Texture(scene_images[0], scene.images[0]->description, copy), parameters).drew)
            return Fail(Result::BackendFailure);
        ++counts.copies;
        parameters.color = scene_images[0]; parameters.depth = scene_images[1];
        parameters.rt_copy = scene_images[2];
        parameters.render_w = int(frame.width); parameters.render_h = int(frame.height);
        parameters.target_w = int(scene.images[0]->description.width / scale);
        parameters.target_h = int(scene.images[0]->description.height / scale);
        for (const auto& draw : draws) {
            parameters.cd_copy = nullptr;
            const auto result = backend.Draw(draw, Texture(images[0], leases[0]->description, draw), parameters);
            if (!result.drew) return Fail(Result::BackendFailure);
            if (!scene.publish(draw, result)) return Fail(Result::PublicationFailure);
            ++counts.batches; counts.triangles += draw.verts.size() / 3;
        }
        ++counts.passes;
        return Result::Ok;
    }
};
DistortionRenderer::DistortionRenderer(Backend& backend, GraphicsResources& resources)
    : impl_(new Impl(backend, resources)) {}
DistortionRenderer::~DistortionRenderer() = default;
bool DistortionRenderer::Valid(const Frame& frame, const std::vector<ps2r::gs::DrawIr>& draws) {
    if (frame.width != 512 || !frame.height || frame.height > 1024 || draws.size() > 16384) return false;
    size_t vertices = 0;
    for (const auto& draw : draws) {
        if (draw.prim_type != 4 || !draw.tex_enabled || draw.use_fst || !draw.gouraud || draw.fog || draw.ctx ||
            draw.tex.psm != 1 || draw.tex.tw != 8 || draw.tex.th != 8 || draw.tex.tbw != 256 ||
            draw.tex.tcc || draw.tex.tfx || draw.tex.mxl || draw.tex.mmin > 1 ||
            draw.tex.wms != 1 || draw.tex.wmt != 1 || draw.raster.frame.psm || draw.raster.zbuf.psm ||
            draw.src_frame != frame.source_frame || draw.verts.empty() || draw.verts.size() % 3 ||
            draw.vertex_count != draw.verts.size()) return false;
        vertices += draw.verts.size();
        if (vertices > 4096 * 12 * 18) return false;
        for (const auto& vertex : draw.verts)
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z) ||
                !std::isfinite(vertex.s) || !std::isfinite(vertex.t) || vertex.q != 1) return false;
    }
    return true;
}
DistortionRenderer::Result DistortionRenderer::Draw(const Frame& frame,
    const MaterialState::Environment& environment, const std::vector<ps2r::gs::DrawIr>& draws, const Scene& scene) {
    auto& p = *impl_;
    if (p.faulted) return Result::Faulted;
    if (!Valid(frame, draws)) return Result::InvalidInput;
    ps2r::gs::DrawIr copy;
    if (ScreenDraw::DistortionCopy(frame.height, frame.source_frame, environment, copy) != ScreenDraw::Result::Ok)
        return Result::InvalidInput;
    std::array<SurfaceId, 3> scene_images;
    if (!p.SceneImages(frame, scene, scene_images)) return Result::InvalidScene;
    try {
        if (!p.Allocate(scene.images[0]->description.scale, frame.height)) return p.Fail(Result::ResourceFailure);
        return p.Execute(frame, copy, draws, scene, scene_images);
    } catch (...) { return p.Fail(Result::BackendFailure); }
}
void DistortionRenderer::ResetAfterQuiescence() { impl_->Release(); impl_->faulted = false; }
DistortionRenderer::Counters DistortionRenderer::Counts() const { return impl_->counts; }
}  // namespace title
