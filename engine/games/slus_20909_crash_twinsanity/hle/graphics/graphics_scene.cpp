#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

#include <stdexcept>

namespace title {
using ps2r::gs::RenderTarget;
using ps2r::render::SurfaceId;

GraphicsScene::GraphicsScene(GraphicsSurfaces& surfaces, GraphicsResources& resources,
                             GraphicsRenderer& renderer, ps2r::gs::DrawPipeline& retained)
    : surfaces_(surfaces), resources_(resources), renderer_(renderer), retained_(retained) {}

bool GraphicsScene::ForeignPages(const Frame& frame, uint32_t footprint) const {
    // The retained arm's alias bookkeeping marks every page a writeback
    // deposited (AliasPageSet: cpu_dirty + aliased), including the target's OWN
    // colour writeback at the native scanout of the frame it just drew. That
    // deposit is the attachment's own content (CT32 encodes losslessly): the
    // two views of the page agree, nothing is owed to the attachment, so it is
    // not a dependency of the next owned frame. Pages whose last author is
    // anyone else (an EE upload, another target aliasing the bytes) are.
    const auto& alias = retained_.Cache().AliasPages();
    const auto& vram = retained_.Vram();
    const uint32_t first = frame.color_buffer / ps2r::gs::VramImage::kPageBytes;
    const uint32_t last = (frame.color_buffer + footprint - 1) / ps2r::gs::VramImage::kPageBytes;
    for (uint32_t page = first; page <= last && page < ps2r::gs::VramImage::kPageCount; ++page) {
        if (!alias.IsAliased(page) && !alias.IsCpuDirty(page)) continue;
        const auto& record = vram.Page(page);
        if (record.author != ps2r::gs::PageRecord::Author::GpuTarget ||
            record.author_fbp != frame.color_buffer) return true;
    }
    return false;
}

bool GraphicsScene::Ready(const Frame& frame, const RenderTarget& target) const {
    // An owned draw needs a 32-bit frame target; a capture (Capturable) takes
    // any CT frame format the attachments carry.
    return target.identity.render_psm == 0 && Capturable(frame, target);
}

bool GraphicsScene::Capturable(const Frame& frame, const RenderTarget& target) const {
    const auto& cache = retained_.Cache();
    const uint32_t footprint = ps2r::gs::RenderTargetCache::FootprintBytes(target);
    if (!footprint || ForeignPages(frame, footprint)) return false;
    for (const auto& entry : cache.Ledger().Entries())
        if (ps2r::gs::PendingRtSyncLedger::Overlaps(entry, frame.color_buffer, footprint))
            return false;
    const auto* sequence = retained_.Sequence();
    return !sequence || sequence->GetState() == ps2r::gs::SequenceRecipe::kIdle;
}

void GraphicsScene::SettleBeforeDraw(const Frame& frame, const RenderTarget& target) {
    auto& cache = retained_.Cache();
    const auto* sequence = retained_.Sequence();
    if (cache.Ledger().Empty() || (sequence && sequence->GetState() != ps2r::gs::SequenceRecipe::kIdle)) return;
    const uint32_t footprint = ps2r::gs::RenderTargetCache::FootprintBytes(target);
    for (const auto& entry : cache.Ledger().Entries()) {
        if (!ps2r::gs::PendingRtSyncLedger::Overlaps(entry, frame.color_buffer, footprint)) continue;
        cache.FlushPending(ps2r::gs::PendingRtSyncLedger::Reason::PreDraw, frame.color_buffer, footprint);
        ++predraw_settles_;
        return;
    }
}

GraphicsScene::Result GraphicsScene::Bind(
    Graphics& queue, GraphicsTargets& targets, const Frame& frame, Binding& out) {
    return BindTarget(queue, targets, frame, out, false);
}

GraphicsScene::Result GraphicsScene::BindSky(
    Graphics& queue, GraphicsTargets& targets, const Frame& source, Binding& out, bool preserve) {
    // TR renderer.h HalfSizeBufferPixelOffset=10, sky.cpp StartSkyBuffer.
    // These are source layout keys, never a state/model texture lookup.
    if (!source.width || source.width > 2048 || source.width % 128 ||
        !source.height || source.height > 2048 || source.height % 2)
        return Result::InvalidFrame;
    const uint64_t pixels = uint64_t(source.width) * source.height;
    const uint64_t key = (pixels * 10 / 8192) * 8192;
    if (key + pixels > 4 * 1024 * 1024) return Result::InvalidFrame;
    auto half = source;
    half.color_buffer = uint32_t(key);
    half.width >>= 1;
    half.height >>= 1;
    return BindTarget(queue, targets, half, out, true, preserve);
}

GraphicsScene::Result GraphicsScene::BindTarget(
    Graphics& queue, GraphicsTargets& targets, const Frame& frame, Binding& out, bool sky, bool preserve) {
    if (!frame.width || frame.width > 2048 || frame.width % 64 ||
        !frame.height || frame.height > 2048 || frame.color_buffer % 8192 ||
        frame.depth_buffer % 8192 || frame.color_buffer >= 4 * 1024 * 1024 ||
        frame.depth_buffer >= 4 * 1024 * 1024)
        return Result::InvalidFrame;
    auto& cache = retained_.Cache();
    // Source setup supplies dimensions. This is the same cache allocation the
    // next retained draw acquires, never a parallel scene/color copy.
    auto* existing = cache.Find(frame.color_buffer);
    if (preserve && (!existing || !existing->coherence.has_content ||
                     !existing->coherence.HasValidContent()))
        return Result::UnclosedDependency;
    // Growing a retained target can rescue pixels to its old mirror. Source
    // frame setup must complete any such transition before selecting this
    // group; the typed boundary must not trigger that path.
    if (existing && (frame.width > uint32_t(existing->identity.width) ||
                     frame.height > uint32_t(existing->identity.height)))
        return Result::UnclosedDependency;
    auto& target = existing ? *existing :
        cache.GetOrCreate(frame.color_buffer, int(frame.width), int(frame.height), 0, frame.epoch);
    SettleBeforeDraw(frame, target);
    if (!Ready(frame, target)) return Result::UnclosedDependency;
    Version version{frame, {}, {target.views.color, target.views.depth, target.views.rt_copy}, sky};
    Binding binding;
    size_t count = 0;
    for (auto surface : version.surfaces) {
        const auto pin = surfaces_.Pin(surface);
        if (!pin || resources_.RegisterExisting(queue, surfaces_.DescribeSurface(surface), surface, pin,
                                                &binding.handles[count]) != GraphicsResources::Result::Ok) {
            while (count) queue.ReleaseResource(binding.handles[--count]);
            return Result::ResourceFailure;
        }
        ++count;
    }
    Graphics::Lease color;
    if (queue.RetainResource(binding.handles[0], &color) != Graphics::Result::Ok) {
        Release(queue, binding);
        return Result::ResourceFailure;
    }
    version.color = color;
    const std::array<GraphicsTargets::Name, 3> names = sky
        ? std::array<GraphicsTargets::Name, 3>{GraphicsTargets::Name::SkyColor,
            GraphicsTargets::Name::SkyDepth, GraphicsTargets::Name::SkyCopy}
        : std::array<GraphicsTargets::Name, 3>{GraphicsTargets::Name::SceneColor,
            GraphicsTargets::Name::SceneDepth, GraphicsTargets::Name::SceneCopy};
    // All handle descriptors were checked by the resource registry. These
    // operations cannot fail after the compatible actual attachments above.
    for (size_t i = 0; i < names.size(); ++i)
        if (targets.Bind(names[i], binding.handles[i]) != GraphicsTargets::Result::Ok) {
            for (auto name : names) targets.Unbind(name);
            Release(queue, binding);
            return Result::ResourceFailure;
        }
    versions_[color.get()] = std::move(version);
    out = std::move(binding);
    return Result::Ok;
}

void GraphicsScene::Release(Graphics& queue, Binding& binding) {
    for (auto& handle : binding.handles) {
        queue.ReleaseResource(handle);
        handle = {};
    }
}

GraphicsScene::Result GraphicsScene::Draw(
    const ps2r::gs::DrawIr& draw, const std::vector<Graphics::Lease>& bindings) {
    return DrawBound(draw, bindings, false);
}

GraphicsScene::Result GraphicsScene::DrawBound(
    const ps2r::gs::DrawIr& draw, const std::vector<Graphics::Lease>& bindings, bool source_setup) {
    if (bindings.size() < 3 || !bindings[0]) return Result::MissingScene;
    const auto found = versions_.find(bindings[0].get());
    if (found == versions_.end() || found->second.color.lock() != bindings[0])
        return Result::MissingScene;
    const auto& version = found->second;
    const auto& frame = version.frame;
    if (draw.src_frame != frame.source_frame || draw.raster.frame.psm || draw.raster.zbuf.psm ||
        draw.raster.frame.fbw != frame.width)
        return Result::InvalidFrame;
    if (version.sky && (draw.ctx || draw.fog || !draw.raster.test.zte ||
                        draw.raster.test.ztst != 1))
        return Result::UnclosedDependency;
    auto* target = retained_.Cache().Find(frame.color_buffer);
    if (!target || target->views.color.id != version.surfaces[0].id ||
        target->views.depth.id != version.surfaces[1].id ||
        target->views.rt_copy.id != version.surfaces[2].id)
        return Result::StaleScene;
    for (size_t i = 0; i < 3; ++i) {
        SurfaceId surface;
        if (resources_.Resolve(bindings[i], &surface) != GraphicsResources::Result::Ok ||
            surface.id != version.surfaces[i].id) return Result::StaleScene;
    }
    if (!Ready(frame, *target)) return Result::UnclosedDependency;

    // Only the retained host target's clear/coherence protocol is shared.
    // No texture resolver, alias import, packet reader or GsState is invoked.
    auto compatibility = draw;
    compatibility.raster.frame.fbp = frame.color_buffer;
    compatibility.raster.zbuf.zbp = frame.depth_buffer;
    compatibility.tex.tbp = compatibility.tex.cbp = 0;
    for (auto& address : compatibility.tex.mip_tbp) address = 0;
    // Colour-authored shadow depth requires an explicit resource contract;
    // using ordinary scene depth would silently lose OLD SH8 semantics.
    if (retained_.ZsrcRouteAvailable(compatibility)) return Result::UnclosedDependency;
    ps2r::gs::DrawPipeline::Prepared prepared;
    prepared.target = target;
    prepared.render_w = int(frame.width);
    prepared.render_h = int(frame.height);
    if (!version.sky && !source_setup)
        target->PrepareDraw(surfaces_, compatibility, retained_.Present().EeFrameCount(),
                            retained_.Scanout().ScanoutFbp(), &prepared.params);
    else {
        // StartSkyBuffer preserves colour; its accepted type-A tests ALWAYS.
        // Source SetUpFrame independently authors its own rectangle/masks.
        // Neither route permits an automatic whole-frame clear here.
        prepared.params.color = target->views.color;
        prepared.params.depth = target->views.depth;
        prepared.params.scale = target->identity.scale;
    }
    GraphicsRenderer::Frame controls;
    controls.clear_depth = prepared.params.do_frame_clear;
    controls.clear_color = prepared.params.clear_color;
    controls.width = frame.width;
    controls.height = frame.height;
    ps2r::render::DrawResult result;
    if (renderer_.Draw(draw, bindings, controls, &result) != GraphicsRenderer::Result::Ok)
        return Result::DrawFailure;
    // The snapshot generation belongs to source setup, not retained uploads.
    // Keep the epoch already established by prior ordered renderer work; mixing
    // the two counters would corrupt the retained alias-author comparisons.
    retained_.SetSrcFrame(frame.source_frame);
    // CT32 only: Finish updates author/depth history, Cd-copy dirt, current
    // target validity and retained texture eviction; it deposits no mirror.
    retained_.Finish(compatibility, prepared, result);
    return Result::Ok;
}

void GraphicsScene::CollectAfterQuiescence() {
    if (resources_.CollectAfterQuiescence() != GraphicsResources::Result::Ok)
        throw std::runtime_error("Graphics scene resource collection failed");
    for (auto it = versions_.begin(); it != versions_.end();) {
        if (it->second.color.expired()) it = versions_.erase(it);
        else ++it;
    }
    surfaces_.CollectAfterQuiescence();
}

}  // namespace title
