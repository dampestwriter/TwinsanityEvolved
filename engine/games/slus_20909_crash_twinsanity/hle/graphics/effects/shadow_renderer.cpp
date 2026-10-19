#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_renderer.h"

#include <algorithm>

#include "engine/gs/hle/texsrc/texture_source.h"

namespace title {
namespace {
using namespace ps2r::render;
using Step = ShadowDraw::Step;
using Source = ps2r::gs::TextureSource;

StencilState Stencil(uint8_t reference, uint8_t compare, uint8_t write, StencilState::Op operation) {
    StencilState state;
    state.enabled = true;
    state.func = StencilState::kEqual;
    state.ref = reference;
    state.cmp_mask = compare;
    state.write_mask = write;
    state.pass_op = operation;
    return state;
}
Source Texture(SurfaceId image, const SurfaceDesc& description, uint8_t depth, bool linear) {
    Source source;
    source.enabled = true;
    source.owner.kind = Source::OwnerKind::Surface;
    source.owner.surface = image;
    source.owner.fmt_case = depth;
    source.transform.depth_fmt = depth;
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
bool SceneDescription(const Graphics::Lease& image, uint32_t width, uint32_t height,
                      uint8_t scale, SurfaceFormat format, bool color_attachment) {
    if (!image || !image->lifetime) return false;
    const auto& d = image->description;
    return scale && d.scale == scale && d.width >= width * scale && d.height >= height * scale &&
           d.width % scale == 0 && d.height % scale == 0 &&
           d.format == format && (!color_attachment || d.renderable);
}
}  // namespace

struct ShadowRenderer::Impl {
    // Accumulator, its destination copy, inverted depth, its destination copy,
    // ordinary scratch depth, and the winding stencil.
    enum Image : size_t { Accumulator, AccumulatorCopy, Inverse, InverseCopy, Depth, StencilImage, Count };
    Backend& backend;
    GraphicsResources& resources;
    Graphics registry;
    std::array<Graphics::Handle, Count> handles;
    std::array<Graphics::Lease, Count> leases;
    std::array<SurfaceId, Count> images;
    SurfaceDesc description;
    Counters counts;
    bool faulted = false;

    Impl(Backend& device, GraphicsResources& assets) : backend(device), resources(assets) {}
    void Release() {
        for (auto& handle : handles) {
            registry.ReleaseResource(handle);
            handle = {};
        }
        leases = {};
        images = {};
        description = {};
    }
    Result Fail(Result result) {
        ++counts.failures;
        faulted = true;
        return result;
    }
    // The private half-size images are NATIVE (scale 1, the source's half-size
    // buffer resolution) whatever the scene's scale: the backend's strip-count
    // pass rasterises at the logical size into the stencil (its prime rule pins
    // the accumulator native: DrawStripCount and the zsrc coordinate map), so a scaled
    // accumulator would hold the volumes at half size in one quarter -- the s4
    // bug at video.scale=2 (no footprint under Crash). The composite then
    // samples the native accumulator bilinearly into the scaled scene, as the
    // source's half-size texture is sampled into its frame.
    bool Allocate(const ShadowDraw::Frame& frame) {
        const uint32_t width = frame.width / 2, height = frame.height / 2;
        if (description.width == width && description.height == height && description.scale == 1)
            return true;
        Release();
        description = {width, height, SurfaceFormat::Rgba8, true, 1};
        for (size_t i = 0; i < Count; ++i) {
            auto d = description;
            if (i == Depth || i == StencilImage) {
                d.format = i == Depth ? SurfaceFormat::Depth32f : SurfaceFormat::DepthStencil24_8;
                d.renderable = false;
            }
            if (resources.Register(registry, d, &handles[i]) != GraphicsResources::Result::Ok ||
                registry.RetainResource(handles[i], &leases[i]) != Graphics::Result::Ok ||
                resources.Resolve(leases[i], &images[i]) != GraphicsResources::Result::Ok)
                return false;
            ++counts.allocations;
        }
        return true;
    }
    DrawParams Parameters(Image color, Image copy, const ShadowDraw::Frame& frame) const {
        DrawParams params;
        params.color = images[color];
        params.depth = images[Depth];
        params.rt_copy = images[copy];
        params.rt_copy_available = true;
        params.render_w = params.target_w = int(frame.width / 2);
        params.render_h = params.target_h = int(frame.height / 2);
        params.scale = 1;
        return params;
    }
    void CountStrip(const ps2r::gs::DrawIr& draw, DrawParams params, const Rect& full) {
        params.program = StencilProgram::StripCount;
        params.stencil = images[StencilImage];
        params.stencil_state = Stencil(0, 0x80, 0x80, StencilState::kInvert);
        params.zsrc = images[Inverse];
        params.zsrc_armed = true;
        params.zsrc_scale = 1;
        // A strip may be completely clipped. It still constitutes owned work;
        // the following conditional quads touch no pixel when no touch bit is set.
        backend.Draw(draw, {}, params);
        backend.DrawConstQuad({}, images[StencilImage], full, 0, false,
            Stencil(0x80, 0x80, 0x1f, draw.verts.front().r == 8
                ? StencilState::kIncrWrap : StencilState::kDecrWrap));
        backend.DrawConstQuad({}, images[StencilImage], full, 0, false,
            Stencil(0x80, 0x80, 0x80, StencilState::kInvert));
    }
    void Materialize(const Rect& full) {
        for (uint8_t value = 0; value < 32; ++value) {
            const uint32_t byte = uint32_t(value) * 8;
            backend.DrawConstQuad(images[Accumulator], images[StencilImage], full,
                0xff000000u | byte * 0x010101u, false,
                Stencil(value, 0x1f, 0, StencilState::kKeep));
        }
    }
    Result Execute(const ShadowDraw::Frame& frame, const std::vector<ShadowDraw::Command>& commands,
                   const Scene& scene, const std::array<SurfaceId, 3>& scene_images, uint8_t scale) {
        const Rect full{0, 0, int(description.width), int(description.height)};
        backend.ClearStencil(images[StencilImage], 15);
        for (const auto& command : commands) {
            auto params = Parameters(Accumulator, AccumulatorCopy, frame);
            Source source;
            switch (command.step) {
            case Step::Clear: break;
            case Step::SeedAlpha:
                source = Texture(scene_images[0], scene.images[0]->description, 0, false);
                break;
            case Step::CopyDepth:
                params = Parameters(Inverse, InverseCopy, frame);
                source = Texture(scene_images[1], scene.images[1]->description, 1, false);
                break;
            case Step::InvertDepth:
                params = Parameters(Inverse, InverseCopy, frame);
                params.program = StencilProgram::Invert;
                source.enabled = true;
                source.transform.uv_tex_w = source.transform.uv_tex_h = 1;
                source.transform.idx_fmt = 4;
                source.transform.idx_self = 1;
                break;
            case Step::Volume:
                CountStrip(command.draw, params, full);
                continue;
            case Step::Quantize:
                Materialize(full);
                break;
            case Step::Composite:
                // The scene's own target and scale; the accumulator is its texture.
                params.color = scene_images[0];
                params.depth = scene_images[1];
                params.rt_copy = scene_images[2];
                params.render_w = int(frame.width);
                params.render_h = int(frame.height);
                params.target_w = int(scene.images[0]->description.width / scale);
                params.target_h = int(scene.images[0]->description.height / scale);
                params.scale = scale;
                params.has_32bit_content = true;
                source = Texture(images[Accumulator], description, 0, true);
                break;
            }
            // Each operation owns its destination-copy boundary. A fresh tracker
            // also observes the intervening stencil resolve without a stale copy.
            CdCopyState copy;
            params.cd_copy = &copy;
            const auto result = backend.Draw(command.draw, source, params);
            if (!result.drew) return Fail(Result::BackendFailure);
            if (command.step == Step::Composite && !scene.publish(command.draw, result))
                return Fail(Result::PublicationFailure);
        }
        return Result::Ok;
    }
};

ShadowRenderer::ShadowRenderer(Backend& backend, GraphicsResources& resources)
    : impl_(new Impl(backend, resources)) {}
ShadowRenderer::~ShadowRenderer() = default;

ShadowRenderer::Result ShadowRenderer::Draw(const ShadowDraw::Frame& frame,
    const std::vector<ps2r::gs::DrawIr>& volumes, const Scene& scene) {
    auto& p = *impl_;
    if (p.faulted) return Result::Faulted;
    std::vector<ShadowDraw::Command> commands;
    if (ShadowDraw::Build(frame, volumes, commands) != ShadowDraw::Result::Ok)
        return Result::InvalidInput;
    const auto scale = scene.images[0] ? scene.images[0]->description.scale : uint8_t(0);
    if (!scene.publish ||
        !SceneDescription(scene.images[0], frame.width, frame.height, scale, SurfaceFormat::Rgba8, true) ||
        !SceneDescription(scene.images[1], frame.width, frame.height, scale, SurfaceFormat::Depth32f, false) ||
        !SceneDescription(scene.images[2], frame.width, frame.height, scale, SurfaceFormat::Rgba8, false))
        return Result::InvalidScene;
    const auto& color = scene.images[0]->description;
    for (size_t i = 1; i < scene.images.size(); ++i)
        if (scene.images[i]->description.width != color.width ||
            scene.images[i]->description.height != color.height)
            return Result::InvalidScene;
    std::array<SurfaceId, 3> images;
    for (size_t i = 0; i < images.size(); ++i) {
        if (p.resources.Resolve(scene.images[i], &images[i]) != GraphicsResources::Result::Ok)
            return Result::InvalidScene;
        for (size_t j = 0; j < i; ++j)
            if (images[j].id == images[i].id) return Result::InvalidScene;
    }
    if (!p.Allocate(frame)) return p.Fail(Result::ResourceFailure);
    try {
        const auto result = p.Execute(frame, commands, scene, images, scale);
        if (result != Result::Ok) return result;
    } catch (...) {
        return p.Fail(Result::BackendFailure);
    }
    ++p.counts.passes;
    p.counts.strips += volumes.size();
    for (const auto& volume : volumes) p.counts.triangles += volume.verts.size() / 3;
    return Result::Ok;
}
void ShadowRenderer::ResetAfterQuiescence() {
    impl_->Release();
    impl_->faulted = false;
}
ShadowRenderer::Counters ShadowRenderer::Counts() const { return impl_->counts; }
}  // namespace title
