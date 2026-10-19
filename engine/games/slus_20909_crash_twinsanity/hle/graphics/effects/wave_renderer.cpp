#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/wave_renderer.h"

#include "engine/gs/hle/texsrc/texture_source.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"

namespace title {
namespace {
using namespace ps2r::render;
ps2r::gs::TextureSource Texture(SurfaceId image, const SurfaceDesc& description) {
    ps2r::gs::TextureSource source;
    source.enabled = true;
    source.owner.kind = ps2r::gs::TextureSource::OwnerKind::Surface;
    source.owner.surface = image;
    source.transform.uv_tex_w = int(description.width / description.scale);
    source.transform.uv_tex_h = int(description.height / description.scale);
    source.transform.uv_pre_w = source.transform.uv_tex_w;
    source.transform.uv_pre_h = source.transform.uv_tex_h;
    source.transform.draw_scale = description.scale;
    source.sampler.mag_linear = true;
    source.sampler.mmin = 1;
    source.sampler.wms = source.sampler.wmt = 1;
    return source;
}
}
struct WaveRenderer::Impl {
    Backend& backend;
    GraphicsResources& resources;
    Graphics registry;
    std::array<Graphics::Handle, 2> handles;
    std::array<Graphics::Lease, 2> leases;
    std::array<SurfaceId, 2> images;
    SurfaceDesc description;
    Counters counts;
    bool faulted = false;

    Impl(Backend& device, GraphicsResources& owner) : backend(device), resources(owner) {}
    void Release() {
        for (auto& handle : handles) { registry.ReleaseResource(handle); handle = {}; }
        leases = {};
        images = {};
        description = {};
    }
    Result Fail(Result result) { faulted = true; ++counts.failures; return result; }
    bool Allocate(uint32_t height, uint8_t scale) {
        if (description.height == height * scale && description.scale == scale) return true;
        Release();
        description = {uint32_t(512 * scale), height * scale, SurfaceFormat::Rgba8, true, scale};
        for (size_t i = 0; i < handles.size(); ++i) {
            if (resources.Register(registry, description, &handles[i]) != GraphicsResources::Result::Ok ||
                registry.RetainResource(handles[i], &leases[i]) != Graphics::Result::Ok ||
                resources.Resolve(leases[i], &images[i]) != GraphicsResources::Result::Ok)
                return false;
            ++counts.allocations;
        }
        return true;
    }
    bool SceneImages(const WaveGeometry::Grid& grid, const Scene& scene,
                     std::array<SurfaceId, 3>& out) {
        if (!scene.publish) return false;
        for (const auto& image : scene.images) if (!image || !image->lifetime) return false;
        const auto& color = scene.images[0]->description;
        if (!color.scale || color.width < grid.width * color.scale ||
            color.height < grid.height * color.scale || color.width % color.scale ||
            color.height % color.scale || !color.renderable)
            return false;
        for (size_t i = 0; i < out.size(); ++i) {
            const auto& desc = scene.images[i]->description;
            if (desc.width != color.width || desc.height != color.height || desc.scale != color.scale ||
                desc.format != (i == 1 ? SurfaceFormat::Depth32f : SurfaceFormat::Rgba8) ||
                resources.Resolve(scene.images[i], &out[i]) != GraphicsResources::Result::Ok)
                return false;
            for (size_t j = 0; j < i; ++j) if (out[i].id == out[j].id) return false;
        }
        return true;
    }
    Result Execute(const ps2r::gs::DrawIr& copy, const std::vector<ps2r::gs::DrawIr>& draws,
                   const Scene& scene, const std::array<SurfaceId, 3>& scene_images) {
        const auto& color = scene.images[0]->description;
        DrawParams params;
        params.color = images[0];
        params.depth = scene_images[1];
        params.rt_copy = images[1];
        params.rt_copy_available = true;
        params.render_w = params.target_w = 512;
        params.render_h = params.target_h = int(description.height / description.scale);
        params.scale = description.scale;
        params.has_32bit_content = true;
        // Only RGB is copied and subsequently sampled by the wave composite.
        // Initialize unused private alpha so pooled storage is never observed.
        backend.Clear(images[0], {}, 0, 0, kClearColor);
        if (!backend.Draw(copy, Texture(scene_images[0], color), params).drew)
            return Fail(Result::BackendFailure);
        params.color = scene_images[0];
        params.rt_copy = scene_images[2];
        params.target_w = int(color.width / color.scale);
        params.target_h = int(color.height / color.scale);
        const auto source = Texture(images[0], description);
        for (const auto& draw : draws) {
            // A fresh destination read sees prior strips in this same pass.
            params.cd_copy = nullptr;
            const auto result = backend.Draw(draw, source, params);
            if (!scene.publish(draw, result)) return Fail(Result::PublicationFailure);
        }
        return Result::Ok;
    }
};

WaveRenderer::WaveRenderer(Backend& backend, GraphicsResources& resources)
    : impl_(new Impl(backend, resources)) {}
WaveRenderer::~WaveRenderer() = default;
WaveRenderer::Result WaveRenderer::Draw(uint32_t frame, const WaveGeometry::Grid& grid,
                                        const WaveGeometry::Waves& waves, const Scene& scene) {
    auto& p = *impl_;
    if (p.faulted) return Result::Faulted;
    ps2r::gs::DrawIr copy;
    std::vector<ps2r::gs::DrawIr> draws;
    if (grid.width != 512 || grid.strips.size() != 64 ||
        ScreenDraw::WaveCopy(grid.height, frame, copy) != ScreenDraw::Result::Ok ||
        WaveGeometry::Build(grid, waves, frame, draws) != WaveGeometry::Result::Ok)
        return Result::InvalidInput;
    std::array<SurfaceId, 3> images;
    if (!p.SceneImages(grid, scene, images)) return Result::InvalidScene;
    if (!p.Allocate(grid.height, scene.images[0]->description.scale)) return p.Fail(Result::ResourceFailure);
    try {
        const auto result = p.Execute(copy, draws, scene, images);
        if (result != Result::Ok) return result;
    } catch (...) { return p.Fail(Result::BackendFailure); }
    ++p.counts.passes;
    p.counts.strips += draws.size();
    for (const auto& draw : draws) p.counts.triangles += draw.verts.size() / 3;
    return Result::Ok;
}
void WaveRenderer::ResetAfterQuiescence() { impl_->Release(); impl_->faulted = false; }
WaveRenderer::Counters WaveRenderer::Counts() const { return impl_->counts; }
}  // namespace title
