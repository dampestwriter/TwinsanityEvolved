#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"

#include <array>

namespace title {
namespace {
using Draw = ps2r::gs::DrawIr;
using Vertex = ps2r::gs::VertexRec;
bool Height(uint32_t height) { return height && height <= 1024; }
Draw Sprites(uint32_t frame, float source_width, float source_height,
             float width, float height, uint32_t depth) {
    Draw draw;
    draw.src_frame = frame;
    draw.prim_type = 6;
    draw.tex_enabled = draw.use_fst = true;
    draw.raster.frame.fbw = uint32_t(width);
    draw.raster.frame.fbmsk = 0xff000000;
    draw.raster.zbuf.zmsk = true;
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 1;
    draw.raster.colclamp = true;
    draw.raster.scissor = {0, 0, 1024, 1024};
    draw.tex.fst = true;
    draw.tex.tcc = draw.tex.tfx = 1;
    draw.tex.tbw = uint32_t(source_width);
    draw.tex.tw = draw.tex.th = 10;
    draw.tex.texa_ta1 = 128;
    draw.tex.mmin = 1;
    draw.tex.mmag = draw.tex.mmin;
    for (unsigned strip = 0; strip < 16; ++strip) {
        std::array<Vertex, 4> corners{};
        for (unsigned corner = 0; corner < 4; ++corner) {
            auto& vertex = corners[corner];
            const unsigned x = strip + (corner & 1), y = corner >> 1;
            vertex.x = float(x) * width / 16;
            vertex.y = float(y) * height;
            vertex.z = float(depth);
            vertex.uv_u = uint16_t(float(x) * source_width + 8) & 0x3fff;
            vertex.uv_v = uint16_t(float(y) * source_height * 16 + 8) & 0x3fff;
            vertex.s = float(vertex.uv_u) / 16;
            vertex.t = float(vertex.uv_v) / 16;
            vertex.q = 1;
            vertex.r = vertex.g = vertex.b = vertex.a = 128;
            vertex.draw_kick = true;
        }
        draw.verts.insert(draw.verts.end(),
                          {corners[0], corners[1], corners[2], corners[1], corners[3], corners[2]});
    }
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.verts_incl = 1;
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
    return draw;
}
}  // namespace
ScreenDraw::Result ScreenDraw::WaveCopy(uint32_t height, uint32_t frame, Draw& out) {
    if (!Height(height)) return Result::InvalidHeight;
    out = Sprites(frame, 512, float(height), 512, float(height), 0x7000);
    return Result::Ok;
}
ScreenDraw::Result ScreenDraw::DistortionCopy(uint32_t height, uint32_t frame,
                                            const MaterialState::Environment& environment, Draw& out) {
    if (!Height(height)) return Result::InvalidHeight;
    auto draw = Sprites(frame, 512, float(height), 256, 256, 0x7fff);
    draw.ctx = 1;
    draw.raster.frame.psm = 1;
    draw.tex.tw = draw.tex.th = 9;
    draw.raster.colclamp = environment.colour_clamp;
    draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.dimx = environment.dither_matrix;
    draw.raster.fogcol_r = environment.fog_r; draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b; draw.scanmsk = environment.scan_mask;
    out = std::move(draw);
    return Result::Ok;
}
ScreenDraw::Result ScreenDraw::Blur(uint32_t height, uint32_t frame, std::vector<Draw>& out) {
    if (!Height(height)) return Result::InvalidHeight;
    std::vector<Draw> draws;
    draws.push_back(Sprites(frame, 512, float(height), 256, float(height) / 2, 0x7fff));
    draws.push_back(Sprites(frame, 256, float(height) / 2, 512, float(height), 0x7fff));
    auto composite = Sprites(frame, 512, float(height), 512, float(height), 0x7fff);
    composite.tex.tfx = 0; // source copy is modulated by White, including alpha
    composite.alpha_blend = true;
    composite.raster.alpha = {1, 0, 0, 0, 0};
    composite.raster.test.ztst = 3;
    draws.push_back(std::move(composite));
    out = std::move(draws);
    return Result::Ok;
}
ScreenDraw::Result ScreenDraw::Filter(uint32_t height, uint32_t frame, bool after_blur, Draw& out) {
    if (!Height(height)) return Result::InvalidHeight;
    auto draw = Sprites(frame, 512, float(height), 512, float(height), 0x10000);
    draw.raster.frame.fbmsk = 0x80000000;
    draw.raster.test.ztst = 3;
    draw.alpha_blend = true;
    draw.raster.alpha = {1, 0, 0, 0, 0};
    draw.tex.psm = 0x1b;
    draw.tex.cld = 1;
    draw.tex.mmin = after_blur ? 1 : 0;
    draw.tex.mmag = draw.tex.mmin;
    out = std::move(draw);
    return Result::Ok;
}
}  // namespace title
