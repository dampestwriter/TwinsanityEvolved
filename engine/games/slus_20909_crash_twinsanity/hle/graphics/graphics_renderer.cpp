#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_renderer.h"

#include <array>
#include <cmath>
#include <limits>

#include "engine/gs/hle/texsrc/texture_source.h"

namespace title {

GraphicsRenderer::GraphicsRenderer(ps2r::render::Backend& backend, GraphicsResources& resources)
    : backend_(backend), resources_(resources) {}

void GraphicsRenderer::TextureLevels(std::function<uint8_t(const Graphics::Lease&)> levels) {
    texture_levels_ = std::move(levels);
}

bool GraphicsRenderer::WithTextureLevels(std::function<uint8_t(const Graphics::Lease&)> levels,
                                        const std::function<bool()>& draw) {
    auto previous = std::move(texture_levels_);
    texture_levels_ = std::move(levels);
    try {
        const bool result = draw && draw();
        texture_levels_ = std::move(previous);
        return result;
    } catch (...) {
        texture_levels_ = std::move(previous);
        throw;
    }
}

GraphicsRenderer::Result GraphicsRenderer::Draw(
    const ps2r::gs::DrawIr& original, const std::vector<Graphics::Lease>& bindings) {
    return Draw(original, bindings, Frame{}, nullptr);
}

GraphicsRenderer::Result GraphicsRenderer::Draw(
    const ps2r::gs::DrawIr& original, const std::vector<Graphics::Lease>& bindings,
    const Frame& frame, ps2r::render::DrawResult* result) {
    using Format = ps2r::render::SurfaceFormat;
    const auto reject = [this](Result result) { ++counts_.rejected; return result; };
    if (frame.display_copy &&
        (!frame.width || !frame.height || frame.clear_color || frame.clear_depth ||
         original.prim_type != 6 || original.ctx != 1 || !original.tex_enabled ||
         !original.use_fst || !original.tex.fst || original.tex.psm != 1 ||
         original.alpha_blend || original.fog || original.raster.fba ||
         !original.raster.zbuf.zmsk || !original.raster.test.zte || original.raster.test.ztst != 1 ||
         original.raster.test.ate || original.raster.test.date || original.raster.frame.fbmsk ||
         !original.raster.colclamp || original.tex.tfx || original.tex.tcc != 1 ||
         original.tex.tw != 10 || original.tex.th != 10))
        return reject(Result::InvalidDraw);
    if (original.verts.empty() || original.verts.size() % 3 ||
        original.vertex_count != original.verts.size() ||
        original.prim_type < 3 || original.prim_type > 6 ||
        original.shuffle_type ||
        original.raster.frame.psm != (frame.display_copy ? 2 : 0) || original.raster.zbuf.psm != 0)
        return reject(Result::InvalidDraw);
    for (const auto& vertex : original.verts)
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.z))
            return reject(Result::InvalidDraw);
    if (bindings.size() != (original.tex_enabled ? 4u : 3u))
        return reject(Result::InvalidBindings);
    for (const auto& binding : bindings)
        if (!binding) return reject(Result::InvalidBindings);
    const auto& color = bindings[GraphicsTargets::Color]->description;
    const auto& depth = bindings[GraphicsTargets::Depth]->description;
    const auto& copy = bindings[GraphicsTargets::Copy]->description;
    constexpr uint32_t maximum = std::numeric_limits<int32_t>::max();
    if (!color.renderable || color.format != Format::Rgba8 ||
        depth.format != Format::Depth32f || copy.format != Format::Rgba8 ||
        !color.width || !color.height || !color.scale ||
        color.width > maximum || color.height > maximum ||
        color.width % color.scale || color.height % color.scale ||
        depth.width != color.width || depth.height != color.height || depth.scale != color.scale ||
        copy.width != color.width || copy.height != color.height || copy.scale != color.scale ||
        bindings[GraphicsTargets::Color]->lifetime == bindings[GraphicsTargets::Copy]->lifetime)
        return reject(Result::InvalidBindings);
    if ((frame.clear_color && !frame.clear_depth) || bool(frame.width) != bool(frame.height) ||
        frame.width > color.width / color.scale || frame.height > color.height / color.scale)
        return reject(Result::InvalidBindings);
    ps2r::gs::TextureSource texture;
    if (original.tex_enabled) {
        const auto& source = bindings[GraphicsTargets::Texture]->description;
        // FinishSky samples an owned render target. Type-A geometry samples
        // an image converted at load time; it never consults a texture slot.
        const auto& t = original.tex;
        if (source.format != Format::Rgba8 || !source.scale ||
            !source.width || !source.height || source.width % source.scale ||
            source.width > maximum || source.height > maximum ||
            source.height % source.scale ||
            bindings[GraphicsTargets::Texture]->lifetime == bindings[GraphicsTargets::Color]->lifetime ||
            bindings[GraphicsTargets::Texture]->lifetime == bindings[GraphicsTargets::Copy]->lifetime)
            return reject(Result::UnsupportedTexture);
        uint8_t levels = 0;
        if (!source.renderable) {
            try { if (texture_levels_) levels = texture_levels_(bindings[GraphicsTargets::Texture]); }
            catch (...) { return reject(Result::UnsupportedTexture); }
        }
        if (source.renderable) {
            if (!original.use_fst || !t.fst || t.psm != (frame.display_copy ? 1 : 0) ||
                t.mmag || t.mmin || t.mxl)
                return reject(Result::UnsupportedTexture);
        } else if (levels) {
            // Load-owned assets supply the complete level chain. UI type-D
            // uses clamp with STQ (Sprite) or texel coordinates (Text); both
            // preserve the source's signed LOD and explicit filter controls.
            // The allocation's extent may differ from the declared 1<<tw/th:
            // a texture pack's replacement at another resolution. The sampler
            // divides by the DECLARED size below (uv_tex_w/h), never the surface's.
            if (source.scale != 1 || original.use_fst != t.fst ||
                (t.psm != 19 && t.psm != 0) || t.tw > 10 || t.th > 10 ||
                levels > 7 || t.mxl >= levels || (t.mmin > 1 && t.mmin != 4) ||
                (t.mmin == 4 && levels < 2) || t.wms > 1 || t.wmt > 1 ||
                t.tcc > 1 || t.tfx > 3)
                return reject(Result::UnsupportedTexture);
            if (t.mmin == 4)
                texture.owner.levels = texture.transform.mip_levels = t.mxl;
            if (!t.fst)
                for (const auto& vertex : original.verts)
                    if (!std::isfinite(vertex.s) || !std::isfinite(vertex.t) || !std::isfinite(vertex.q))
                        return reject(Result::InvalidDraw);
        } else if (original.use_fst || t.fst) {
            // MovieGraphics' owned RGB32 picture. Source TW/TH=10 describes
            // the old transfer arena, not the decoded allocation. FST vertices
            // are already texels, so divide by the actual owned image extent.
            if (!original.use_fst || !t.fst || source.scale != 1 ||
                source.width > 4096 || source.height > 4096 ||
                t.psm || t.tw > 10 || t.th > 10 || t.mmag || t.mmin || t.mxl ||
                t.wms > 1 || t.wmt > 1 || t.tcc > 1 || t.tfx > 3)
                return reject(Result::UnsupportedTexture);
        } else {
            // Owned type01/A STQ textures. Mip levels come from their load-time
            // owner, never TBP/MIPTBP addresses or an inferred texture alias.
            if (source.scale != 1 || original.use_fst || t.fst || (t.psm != 19 && t.psm != 0) ||
                t.tw > 10 || t.th > 10 || source.width != (1u << t.tw) ||
                source.height != (1u << t.th) || (t.mmin > 1 && t.mmin != 4) || t.wms || t.wmt ||
                t.tcc > 1 || t.tfx > 3 || t.mxl > 6)
                return reject(Result::UnsupportedTexture);
            if (t.psm != 19 || t.tw > 7 || t.th > 7 || t.mxl > 4 || t.mmin > 1)
                return reject(Result::UnsupportedTexture);
            for (const auto& vertex : original.verts)
                if (!std::isfinite(vertex.s) || !std::isfinite(vertex.t) || !std::isfinite(vertex.q))
                    return reject(Result::InvalidDraw);
        }
        texture.enabled = true;
        texture.owner.kind = source.renderable ? ps2r::gs::TextureSource::OwnerKind::Surface
                                               : ps2r::gs::TextureSource::OwnerKind::Decoded;
        texture.owner.owned = !source.renderable;   // every typed texture is a load-owned asset
        // Rendered sources: the allocation's extent. Load-owned assets: the
        // DECLARED texel space (1<<tw/th) -- equal to the allocation's extent
        // unless a texture pack replaced the pixels at another resolution.
        const bool declared = !source.renderable && !(original.use_fst && t.fst && !levels);
        texture.transform.uv_tex_w = declared ? int32_t(1u << t.tw) : int32_t(source.width / source.scale);
        texture.transform.uv_tex_h = declared ? int32_t(1u << t.th) : int32_t(source.height / source.scale);
        texture.transform.uv_pre_w = texture.transform.uv_tex_w;
        texture.transform.uv_pre_h = texture.transform.uv_tex_h;
        texture.transform.draw_scale = source.scale;
        texture.sampler.wms = source.renderable ? 1 : t.wms;
        texture.sampler.wmt = source.renderable ? 1 : t.wmt;
        texture.sampler.mag_linear = t.mmag;
        texture.sampler.mmin = t.mmin;
        // BUG-084: keep signed LOD, including the base-only Sky family.
        texture.transform.mip_k = t.k;
        texture.transform.mip_l = t.l;
        texture.transform.mip_lcm = t.lcm;
    }
    std::array<ps2r::render::SurfaceId, 4> surfaces{};
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (resources_.Resolve(bindings[i], &surfaces[i]) != GraphicsResources::Result::Ok) {
            ++counts_.failures;
            return Result::BackendFailed;
        }
    }
    ps2r::render::DrawParams params;
    params.color = surfaces[GraphicsTargets::Color];
    params.depth = surfaces[GraphicsTargets::Depth];
    params.rt_copy = surfaces[GraphicsTargets::Copy];
    params.rt_copy_available = true;
    params.render_w = params.target_w = int32_t(color.width / color.scale);
    params.render_h = params.target_h = int32_t(color.height / color.scale);
    params.scale = color.scale;
    if (frame.width) {
        params.render_w = int32_t(frame.width);
        params.render_h = int32_t(frame.height);
    }
    params.do_frame_clear = frame.clear_depth;
    params.clear_color = frame.clear_color;
    params.has_32bit_content = !frame.display_copy;
    // Null cd_copy requests a fresh destination read for every relevant draw.
    // No stale content/alias epoch can reuse an older alpha/FBA input.
    params.cd_copy = nullptr;
    params.inherited_uniforms = 0;
    if (original.tex_enabled) texture.owner.surface = surfaces[GraphicsTargets::Texture];

    // Addresses are source audit metadata, never backend resource identities.
    // Clear them at this boundary to make accidental address routing impossible.
    auto draw = original;
    draw.raster.frame.fbp = 0;
    draw.raster.zbuf.zbp = 0;
    draw.tex.tbp = draw.tex.cbp = 0;
    for (auto& address : draw.tex.mip_tbp) address = 0;
    // A backend may legitimately cull a fully clipped draw. Submission success
    // is not a promise that a pixel was written, and never triggers a replay.
    try {
        const auto delivered = backend_.Draw(draw, texture, params);
        if (result) *result = delivered;
    }
    catch (...) { ++counts_.failures; return Result::BackendFailed; }
    ++counts_.draws;
    return Result::Ok;
}

GraphicsRenderer::Counters GraphicsRenderer::Counts() const { return counts_; }

}  // namespace title
