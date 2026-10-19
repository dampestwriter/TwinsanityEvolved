#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_intent.h"

#include <array>
#include <utility>

namespace title {

SkyIntent::Result SkyIntent::Build(uint32_t width, uint32_t height,
                                  const MaterialState::Environment& environment, Output& out) {
    // TR sky.cpp:149..202 at 38d533d0033b48b5d6b49880c49f5eeeade0f38e.
    // NTSC generated body [0x1b9f30,0x1ba2bc). A non-multiple of 32 intentionally
    // leaves the final partial strip untouched; do not stretch to fill it.
    const uint64_t pixels = uint64_t(width) * height;
    if (width < 128 || width > 1024 || height < 2 || height > 1024 ||
        pixels > (4 * 1024 * 1024) / 11 || environment.scan_mask > 3)
        return Result::UnsupportedFrame;
    Output result;
    result.width = width;
    result.height = height;
    auto& draw = result.draw;
    draw.prim_type = 6;
    draw.tex_enabled = draw.use_fst = draw.tex.fst = true;
    draw.ctx = 1;
    draw.scanmsk = environment.scan_mask;
    // These byte addresses are source audit metadata only. GraphicsTargets
    // supplies identities; GraphicsRenderer clears addresses before submission.
    draw.raster.frame = {uint32_t(pixels >> 12) * 8192, (width >> 6) * 64, 0, 0};
    draw.raster.zbuf = {uint32_t(pixels * 6 >> 13) * 8192, 0, false};
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 1;
    draw.raster.scissor = {0, 0, 1024, 1024}; // inclusive source endpoints
    draw.raster.colclamp = true;
    draw.raster.pabe = environment.per_pixel_alpha;
    draw.raster.dimx = environment.dither_matrix;
    draw.raster.fogcol_r = environment.fog_r;
    draw.raster.fogcol_g = environment.fog_g;
    draw.raster.fogcol_b = environment.fog_b;
    draw.tex.tbp = uint32_t(pixels * 10 >> 8) * 256;
    draw.tex.tbw = (width >> 7) * 64;
    draw.tex.tw = draw.tex.th = 10;
    draw.tex.tcc = 1;
    const uint32_t strips = width / 32;
    draw.verts.reserve(strips * 6);
    const auto vertex = [](uint32_t x, uint32_t y) {
        ps2r::gs::VertexRec v{};
        v.x = float(x);
        v.y = float(y);
        v.s = float(x) * 0.5f;
        v.t = float(y) * 0.5f;
        v.q = 1;
        v.uv_u = uint16_t(x * 8);
        v.uv_v = uint16_t(y * 8);
        v.r = v.g = v.b = 128;
        v.a = 0; // source GreyQOne, not opaque alpha or a texture copy
        v.draw_kick = 1;
        return v;
    };
    for (uint32_t strip = 0; strip != strips; ++strip) {
        const uint32_t left = strip * 32, right = left + 32;
        const auto tl = vertex(left, 0), tr = vertex(right, 0);
        const auto bl = vertex(left, height), br = vertex(right, height);
        draw.verts.insert(draw.verts.end(), {tl, tr, bl, tr, br, bl});
    }
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    draw.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
    out = std::move(result);
    return Result::Ok;
}

SkyIntent::Result SkyIntent::Capture(const MaterialState::Read& read, uint32_t entry,
                                    const MaterialState::Environment& environment, Output& out) {
    if (entry != 0 && entry != kAddress) return Result::UnsupportedEntry;
    std::array<uint8_t, 8> dimensions{};
    try {
        if (!read || !read(MaterialState::kDisplayWidth, dimensions.data(), dimensions.size()))
            return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    const auto word = [&](size_t offset) {
        const auto* p = dimensions.data() + offset;
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    return Build(word(0), word(4), environment, out);
}

}  // namespace title
