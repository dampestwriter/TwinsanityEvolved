#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

namespace title {
GraphicsScene::Result GraphicsScene::StartFrame(const FrameClear::Output& intent, const Frame& frame) {
    const auto& draw = intent.draw;
    if (intent.width != frame.width || intent.height != frame.height ||
        draw.raster.frame.fbp != frame.color_buffer || draw.raster.zbuf.zbp != frame.depth_buffer ||
        draw.raster.frame.fbw != frame.width || draw.raster.frame.psm || draw.raster.zbuf.psm ||
        draw.ctx != 1 || draw.prim_type != 6 || draw.tex_enabled || draw.fog || draw.alpha_blend ||
        draw.raster.fba || draw.raster.dthe || draw.scanmsk || !draw.raster.colclamp ||
        draw.raster.test.ate || draw.raster.test.date ||
        !draw.raster.test.zte || draw.raster.test.ztst != 1 ||
        (draw.raster.frame.fbmsk != 0 && draw.raster.frame.fbmsk != UINT32_MAX))
        return Result::InvalidFrame;
    const bool full = !draw.verts.empty() && draw.bbox[0] <= 0 && draw.bbox[1] <= 0 &&
        draw.bbox[3] >= float(frame.width) && draw.bbox[4] >= float(frame.height);
    auto* existing = retained_.Cache().Find(frame.color_buffer);
    // A masked/partial producer must not fabricate inherited color or depth.
    // Allocation initialization is sufficient only for complete overwrites.
    if ((!full || draw.raster.frame.fbmsk) &&
        (!existing || !existing->coherence.has_content || !existing->coherence.HasValidContent()))
        return Result::UnclosedDependency;
    if ((!full || draw.raster.zbuf.zmsk) &&
        (!existing || !existing->coherence.last_z_wrote ||
         existing->coherence.last_zbp != frame.depth_buffer || existing->coherence.last_zpsm))
        return Result::UnclosedDependency;
    Graphics queue;
    GraphicsTargets targets(queue);
    Binding binding;
    const auto bound = Bind(queue, targets, frame, binding);
    if (bound != Result::Ok) return bound;
    std::vector<Graphics::Lease> leases;
    for (const auto& handle : binding.handles) {
        Graphics::Lease lease;
        if (queue.RetainResource(handle, &lease) != Graphics::Result::Ok) return Result::ResourceFailure;
        leases.push_back(std::move(lease));
    }
    if (!draw.verts.empty()) {
        auto ordered = draw;
        ordered.src_frame = frame.source_frame;
        const auto result = DrawBound(ordered, leases, true);
        if (result != Result::Ok) return result;
    }
    auto& history = retained_.Cache().Find(frame.color_buffer)->coherence;
    // A source frame is complete even when both masks preserve all pixels.
    // Prevent the next retained draw from inventing an extra whole-frame clear.
    history.last_clear_frame = retained_.Present().EeFrameCount();
    history.needs_frame_clear = false;
    ++frame_setups_;
    return Result::Ok;
}

uint64_t GraphicsScene::FrameSetups() const { return frame_setups_; }

GraphicsScene::Result GraphicsScene::ConfigureDisplay(const DisplayLayout::Output& layout) {
    if (!DisplayLayout::Validate(layout)) return Result::InvalidFrame;
    retained_.OnSetWindow(int(layout.width),int(layout.height),int(layout.width),int(layout.height));
    // Temporary mixed-output host key only. Owned presentation holds the
    // actual converted allocation and never resolves an image by this key.
    retained_.OnSetFrontBuffer(0,layout.width,2);
    ++display_layouts_;
    return Result::Ok;
}
uint64_t GraphicsScene::DisplayLayouts() const { return display_layouts_; }
} // namespace title
