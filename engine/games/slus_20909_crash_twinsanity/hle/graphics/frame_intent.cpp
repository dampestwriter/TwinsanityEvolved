#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_intent.h"

#include <array>

namespace title {

FrameIntent::Result FrameIntent::Build(uint32_t width, uint32_t height,
                                       const MaterialState::Environment& environment, Output& out) {
    // Source fixed layout: 16-bit shown color, 32-bit drawn color/depth.
    // Reject overflow/unsupported source layouts before producing any work.
    const uint64_t pixels = uint64_t(width) * height;
    if (width < 64 || width > 1024 || width % 64 || !height || height > 1024 ||
        pixels * 10 > 4 * 1024 * 1024 || environment.scan_mask > 3)
        return Result::UnsupportedFrame;
    Output next;
    next.width = width; next.height = height;
    auto& draw = next.draw;
    draw.prim_type = 6; draw.ctx = 1;
    draw.tex_enabled = draw.use_fst = draw.tex.fst = true;
    draw.scanmsk = environment.scan_mask;
    draw.raster.frame = {0,width,2,0};
    draw.raster.zbuf = {uint32_t(pixels * 6 / 8192) * 8192,0,true};
    draw.raster.test.zte = true; draw.raster.test.ztst = 1;
    draw.raster.scissor = {0,0,1024,1024};
    draw.raster.colclamp = true;
    draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.fogcol_r = environment.fog_r;
    draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b;
    draw.raster.dthe = 1; draw.raster.dimx = 0x6071243571603524ull;
    draw.tex.tbp = uint32_t(pixels * 2 / 256) * 256;
    draw.tex.tbw = width; draw.tex.psm = 1;
    draw.tex.tw = draw.tex.th = 10; draw.tex.tcc = 1;
    const auto vertex = [](uint32_t x,uint32_t y) {
        ps2r::gs::VertexRec v{};
        v.x = float(x); v.y = float(y); v.z = 8388607.0f; v.q = 1;
        // Source uses half-texel UV endpoints in its 10-bit texture space.
        v.uv_u = uint16_t((x * 16 + 8) & 0x3fff);
        v.uv_v = uint16_t((y * 16 + 8) & 0x3fff);
        v.s = float(v.uv_u) / 16; v.t = float(v.uv_v) / 16;
        v.r = v.g = v.b = 128; v.draw_kick = 1;
        return v;
    };
    draw.verts.reserve(width / 32 * 6);
    for (uint32_t x = 0; x < width; x += 32) {
        const auto tl = vertex(x,0), tr = vertex(x+32,0);
        const auto bl = vertex(x,height), br = vertex(x+32,height);
        draw.verts.insert(draw.verts.end(),{tl,tr,bl,tr,br,bl});
    }
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(),draw.verts.size());
    draw.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(),draw.verts.size(),draw.bbox);
    out = std::move(next);
    return Result::Ok;
}

FrameIntent::Result FrameIntent::Capture(const MaterialState::Read& read, uint32_t entry,
                                         const MaterialState::Environment& environment, Output& out) {
    if (entry && entry != kAddress) return Result::UnsupportedEntry;
    std::array<uint32_t,2> dimensions{}, formats{};
    try {
        if (!read || !read(MaterialState::kDisplayWidth,dimensions.data(),sizeof(dimensions)) ||
            !read(MaterialState::kDisplayPixelBytes,formats.data(),sizeof(formats)))
            return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    if (formats[0] != 2 || formats[1] != 4) return Result::UnsupportedFrame;
    return Build(dimensions[0],dimensions[1],environment,out);
}
} // namespace title
