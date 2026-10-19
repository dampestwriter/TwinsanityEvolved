#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_renderer.h"

#include <cstring>

#include "engine/gs/hle/texsrc/texture_source.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"

namespace title {
namespace {
using namespace ps2r::render;
using Source = ps2r::gs::TextureSource;
Source Texture(SurfaceId image, const SurfaceDesc& description, bool linear) {
    Source source;
    source.enabled = true;
    source.owner.kind = Source::OwnerKind::Surface;
    source.owner.surface = image;
    source.transform.uv_tex_w = int(description.width / description.scale);
    source.transform.uv_tex_h = int(description.height / description.scale);
    source.transform.uv_pre_w = source.transform.uv_tex_w;
    source.transform.uv_pre_h = source.transform.uv_tex_h;
    source.transform.draw_scale = description.scale;
    source.sampler.mag_linear = linear;
    source.sampler.mmin = linear ? 1 : 0;
    source.sampler.wms = source.sampler.wmt = 1;
    return source;
}
}  // namespace

void ScreenRenderer::ClutRows(const ScreenPalettes::Palette& storage, std::array<uint8_t, 256 * 2 * 4>& out) {
    ScreenPalettes::Palette logical;
    ScreenPalettes::Logical(storage, logical);
    for (size_t index = 0; index < logical.size(); ++index) {
        const uint32_t colour = logical[index];
        const uint8_t alpha = uint8_t(colour >> 24);
        uint8_t* scaled = &out[index * 4];
        uint8_t* raw = &out[(256 + index) * 4];
        scaled[0] = raw[0] = uint8_t(colour);
        scaled[1] = raw[1] = uint8_t(colour >> 8);
        scaled[2] = raw[2] = uint8_t(colour >> 16);
        scaled[3] = alpha >= 128 ? 255 : uint8_t(alpha * 2);
        raw[3] = alpha;
    }
}

struct ScreenRenderer::Impl {
    enum Image : size_t { Half, Copy, Clut, Index, Count };
    Backend& backend;
    GraphicsResources& resources;
    Graphics registry;
    std::array<Graphics::Handle, Count> handles;
    std::array<Graphics::Lease, Count> leases;
    std::array<SurfaceId, Count> images;
    SurfaceDesc copy_description;
    bool clut_valid = false;
    ScreenPalettes::Palette clut_palette{};
    Counters counts;
    bool faulted = false;

    Impl(Backend& device, GraphicsResources& owner) : backend(device), resources(owner) {}
    void Release() {
        for (auto& handle : handles) { registry.ReleaseResource(handle); handle = {}; }
        leases = {};
        images = {};
        copy_description = {};
        clut_valid = false;
    }
    Result Fail(Result result) { faulted = true; ++counts.failures; return result; }
    bool Allocate(Image which, const SurfaceDesc& description) {
        if (leases[which] && leases[which]->description.width == description.width &&
            leases[which]->description.height == description.height &&
            leases[which]->description.scale == description.scale &&
            leases[which]->description.renderable == description.renderable)
            return true;
        registry.ReleaseResource(handles[which]);
        handles[which] = {}; leases[which] = {}; images[which] = {};
        if (which == Clut) clut_valid = false;
        if (resources.Register(registry, description, &handles[which]) != GraphicsResources::Result::Ok ||
            registry.RetainResource(handles[which], &leases[which]) != Graphics::Result::Ok ||
            resources.Resolve(leases[which], &images[which]) != GraphicsResources::Result::Ok)
            return false;
        ++counts.allocations;
        return true;
    }
    bool SceneImages(uint32_t height, const Scene& scene, std::array<SurfaceId, 3>& out) {
        if (!scene.publish) return false;
        for (const auto& image : scene.images) if (!image || !image->lifetime) return false;
        const auto& color = scene.images[0]->description;
        if (!color.scale || color.width != 512 * color.scale || color.height < height * color.scale ||
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
    DrawParams SceneParameters(const Scene& scene, const std::array<SurfaceId, 3>& scene_images, uint32_t height) {
        const auto& color = scene.images[0]->description;
        DrawParams params;
        params.color = scene_images[0];
        params.depth = scene_images[1];
        params.rt_copy = scene_images[2];
        params.rt_copy_available = true;
        params.render_w = 512;
        params.render_h = int(height);
        params.target_w = int(color.width / color.scale);
        params.target_h = int(color.height / color.scale);
        params.scale = color.scale;
        params.has_32bit_content = true;
        return params;
    }
    Result Blur(uint32_t frame, const Pass& pass, const Scene& scene, const std::array<SurfaceId, 3>& scene_images) {
        const auto& color = scene.images[0]->description;
        const uint8_t scale = color.scale;
        std::vector<ps2r::gs::DrawIr> draws;
        if (ScreenDraw::Blur(pass.height, frame, draws) != ScreenDraw::Result::Ok || draws.size() != 3)
            return Result::InvalidInput;
        const SurfaceDesc half{uint32_t(256 * scale), (pass.height / 2) * scale, SurfaceFormat::Rgba8, true, scale};
        const SurfaceDesc copy{uint32_t(512 * scale), pass.height * scale, SurfaceFormat::Rgba8, true, scale};
        if (!Allocate(Half, half) || !Allocate(Copy, copy)) return Fail(Result::ResourceFailure);
        copy_description = copy;
        // The retail shrink/stretch write colour only into buffers whose alpha
        // byte is whatever VRAM held; the composite's source alpha is the
        // intent's "replace the far pixels" (alpha 0 -> (Cd - Cs) * 0 + Cs).
        backend.Clear(images[Half], {}, 0, 0, kClearColor);
        backend.Clear(images[Copy], {}, 0, 0, kClearColor);
        DrawParams params;
        params.scale = scale;
        params.has_32bit_content = true;
        params.color = images[Half];
        params.render_w = params.target_w = 256;
        params.render_h = params.target_h = int(pass.height / 2);
        if (!backend.Draw(draws[0], Texture(scene_images[0], color, true), params).drew)
            return Fail(Result::BackendFailure);
        params.color = images[Copy];
        params.render_w = params.target_w = 512;
        params.render_h = params.target_h = int(pass.height);
        if (!backend.Draw(draws[1], Texture(images[Half], half, true), params).drew)
            return Fail(Result::BackendFailure);
        auto composite = SceneParameters(scene, scene_images, pass.height);
        CdCopyState tracker;
        composite.cd_copy = &tracker;
        const auto result = backend.Draw(draws[2], Texture(images[Copy], copy, true), composite);
        if (!result.drew) return Fail(Result::BackendFailure);
        if (!scene.publish(draws[2], result)) return Fail(Result::PublicationFailure);
        ++counts.blur_passes;
        return Result::Ok;
    }
    Result Filter(uint32_t frame, const Pass& pass, const Scene& scene, const std::array<SurfaceId, 3>& scene_images) {
        const auto& color = scene.images[0]->description;
        const uint8_t scale = color.scale;
        ps2r::gs::DrawIr draw;
        if (ScreenDraw::Filter(pass.height, frame, pass.blur, draw) != ScreenDraw::Result::Ok)
            return Result::InvalidInput;
        const SurfaceDesc clut{256, 2, SurfaceFormat::Rgba8, false, 1};
        const SurfaceDesc index{uint32_t(512 * scale), pass.height * scale, SurfaceFormat::Rgba8, true, scale};
        if (!Allocate(Clut, clut) || !Allocate(Index, index)) return Fail(Result::ResourceFailure);
        if (!clut_valid || clut_palette != pass.palette) {
            std::array<uint8_t, 256 * 2 * 4> rows{};
            ClutRows(pass.palette, rows);
            backend.Upload(images[Clut], Rect{0, 0, 256, 2}, rows.data(), 256 * 4);
            clut_palette = pass.palette;
            clut_valid = true;
            ++counts.palette_uploads;
        }
        // Step 1, the palette image: every pixel's entry clut[Z[15:8]] into a
        // private image, sampling the scene depth with no attachment of it
        // (sampling an attached depth is a feedback loop the GL declines).
        // Step 2, the retail strips: that image DECAL over the scene under the
        // GREATER test at 0x10000 and (Cd - Cs) * As + Cs -- the same draw the
        // source asks for, its PSMT8H read answered by the owned image.
        const auto& depth = scene.images[1]->description;
        auto lookup = draw;
        lookup.alpha_blend = false;
        lookup.raster.alpha = {};
        lookup.raster.frame.fbmsk = 0;
        lookup.raster.test.zte = false;
        lookup.raster.test.ztst = 1;
        lookup.raster.zbuf.zmsk = true;
        lookup.tex.psm = 0;
        lookup.tex.cld = 0;
        auto source = Texture(scene_images[1], depth, false);
        source.owner.fmt_case = 7;
        source.owner.clut = images[Clut];
        source.transform.idx_fmt = 7;
        source.transform.idx_self = kDepthShift;
        DrawParams params;
        params.scale = scale;
        params.has_32bit_content = true;
        params.color = images[Index];
        params.clut = images[Clut];
        params.render_w = params.target_w = 512;
        params.render_h = params.target_h = int(pass.height);
        if (!backend.Draw(lookup, source, params).drew) return Fail(Result::BackendFailure);
        auto composite = SceneParameters(scene, scene_images, pass.height);
        CdCopyState tracker;
        composite.cd_copy = &tracker;
        // The retail strips carry the half-texel UV offset of a 1:1 copy; with
        // NEAREST sampling the pixel centre then lands on the texel boundary
        // (u = x + 1) and the host reads the diagonal neighbour. The intent is
        // pixel (x,y) through ITS OWN entry: sample the owned image at the
        // texel centre. The published record stays the source's draw.
        auto aligned = draw;
        for (auto& vertex : aligned.verts) {
            vertex.uv_u = uint16_t(vertex.uv_u - 8);
            vertex.uv_v = uint16_t(vertex.uv_v - 8);
        }
        const auto result = backend.Draw(aligned, Texture(images[Index], index, false), composite);
        if (!result.drew) return Fail(Result::BackendFailure);
        if (!scene.publish(draw, result)) return Fail(Result::PublicationFailure);
        ++counts.filter_draws;
        return Result::Ok;
    }
};

ScreenRenderer::ScreenRenderer(Backend& backend, GraphicsResources& resources)
    : impl_(new Impl(backend, resources)) {}
ScreenRenderer::~ScreenRenderer() = default;
ScreenRenderer::Result ScreenRenderer::Draw(uint32_t frame, const Pass& pass, const Scene& scene) {
    auto& p = *impl_;
    if (p.faulted) return Result::Faulted;
    if (!pass.height || pass.height > 1024 || (pass.height & 1) || !(pass.blur || pass.filter))
        return Result::InvalidInput;
    std::array<SurfaceId, 3> images;
    if (!p.SceneImages(pass.height, scene, images)) return Result::InvalidScene;
    try {
        if (pass.blur) {
            const auto result = p.Blur(frame, pass, scene, images);
            if (result != Result::Ok) return result;
        }
        if (pass.filter) {
            const auto result = p.Filter(frame, pass, scene, images);
            if (result != Result::Ok) return result;
        }
    } catch (...) { return p.Fail(Result::BackendFailure); }
    ++p.counts.passes;
    return Result::Ok;
}
void ScreenRenderer::ResetAfterQuiescence() { impl_->Release(); impl_->faulted = false; }
ScreenRenderer::Counters ScreenRenderer::Counts() const { return impl_->counts; }
}  // namespace title
