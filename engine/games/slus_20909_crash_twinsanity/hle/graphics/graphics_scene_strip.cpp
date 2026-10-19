#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_scene.h"

namespace title {
bool GraphicsScene::CanDrawStrip(const StripIntent::Output& intent,const Frame& frame) {
    const auto& d = intent.draw;
    const auto& a = d.raster.alpha;
    return frame.width && frame.height && frame.epoch && intent.bucket == 24 &&
        d.prim_type == 4 && !d.ctx && d.gouraud && d.alpha_blend &&
        !d.tex_enabled && !d.fog && !d.shuffle_type && d.raster.fba &&
        !d.raster.frame.psm && !d.raster.frame.fbmsk && !d.raster.zbuf.psm &&
        d.raster.frame.fbw == frame.width && d.raster.frame.fbp == frame.color_buffer &&
        d.raster.zbuf.zbp == frame.depth_buffer && d.raster.zbuf.zmsk &&
        !d.raster.test.ate && !d.raster.test.date && d.raster.test.zte && d.raster.test.ztst == 2 &&
        a.a == 0 && a.b == 1 && a.c == 0 && a.d == 1 && a.fix == 0 &&
        !d.verts.empty() && d.verts.size()%3 == 0 && d.vertex_count == d.verts.size();
}
GraphicsScene::Result GraphicsScene::DrawStrip(const StripIntent::Output& intent,const Frame& frame) {
    if (!CanDrawStrip(intent,frame) || intent.draw.src_frame != frame.source_frame)
        return Result::InvalidFrame;
    Graphics queue;
    GraphicsTargets targets(queue);
    Binding binding;
    const auto bound = Bind(queue,targets,frame,binding);
    if (bound != Result::Ok) return bound;
    std::vector<Graphics::Lease> leases;
    for (const auto& handle : binding.handles) {
        Graphics::Lease lease;
        if (queue.RetainResource(handle,&lease) != Graphics::Result::Ok)
            return Result::ResourceFailure;
        leases.push_back(std::move(lease));
    }
    return Draw(intent.draw,leases);
}
} // namespace title
