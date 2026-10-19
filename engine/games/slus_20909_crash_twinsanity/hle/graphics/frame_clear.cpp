#include "games/slus_20909_crash_twinsanity/hle/graphics/frame_clear.h"

#include <array>
#include <cstring>

namespace title {

FrameClear::Result FrameClear::Build(const Input& in, Output& out) {
    const uint64_t pixels = uint64_t(in.screen_width) * in.screen_height;
    // Source's signed arithmetic is exact inside these bounds. Wrapped or
    // negative encoded XYZ coordinates require a separately audited intent.
    if (in.screen_width < 64 || in.screen_width > 1024 || in.screen_width % 64 ||
        !in.screen_height || in.screen_height > 1024 || pixels * 10 > 4 * 1024 * 1024 ||
        in.width > 2048 || in.height > 2048 || in.x > 4095 || in.y > 4095 ||
        in.width > 4095 - in.x || in.height > 4095 - in.y)
        return Result::UnsupportedFrame;
    Output next;
    next.width = in.screen_width; next.height = in.screen_height;
    auto& draw = next.draw;
    draw.prim_type = 6; draw.ctx = 1; draw.use_fst = draw.tex.fst = true;
    draw.raster.frame = {uint32_t(pixels / 4096) * 8192, in.screen_width, 0,
                         in.color ? 0u : UINT32_MAX};
    draw.raster.zbuf = {uint32_t(pixels * 6 / 8192) * 8192, 0, !in.depth};
    draw.raster.test.zte = true; draw.raster.test.ztst = 1;
    draw.raster.scissor = {0, 0, 1024, 1024};
    draw.raster.colclamp = true;
    draw.raster.dimx = 0x6071243571603524ull; // inactive source display matrix
    draw.tex.texa_ta1 = 128;
    const int32_t origin_x = 2048 - int32_t(in.screen_width / 2);
    const int32_t origin_y = 2048 - int32_t(in.screen_height / 2);
    draw.off_x = float(origin_x); draw.off_y = float(origin_y);
    const auto vertex = [&](uint32_t x, uint32_t y) {
        ps2r::gs::VertexRec v{};
        v.x = float(int32_t(x) - origin_x); v.y = float(int32_t(y) - origin_y);
        v.q = 1; v.r = in.red; v.g = in.green; v.b = in.blue; v.draw_kick = 1;
        return v;
    };
    // Source truncates the strip count, including a width shorter than 32.
    for (uint32_t strip = 0; strip < in.width / 32; ++strip) {
        const uint32_t x = in.x + strip * 32;
        const auto tl = vertex(x, in.y), tr = vertex(x + 32, in.y);
        const auto bl = vertex(x, in.y + in.height), br = vertex(x + 32, in.y + in.height);
        draw.verts.insert(draw.verts.end(), {tl, tr, bl, tr, br, bl});
    }
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    draw.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
    out = std::move(next);
    return Result::Ok;
}

FrameClear::Result FrameClear::Capture(const MaterialState::Read& read, const CpuContext& context,
                                       uint32_t entry, Output& out) {
    if (entry && entry != kAddress) return Result::UnsupportedEntry;
    uint32_t address = context.cpuRegs.GPR.r[4].UL[0];
    const auto segment = address >> 29;
    if (segment != 0 && segment != 4 && segment != 5) return Result::InvalidMemory;
    address &= 0x1fffffff;
    std::array<uint8_t, 0x68> bytes{};
    try {
        if (!read || !address || address > 0x02000000 - bytes.size() ||
            !read(address, bytes.data(), bytes.size())) return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    const auto word = [&](size_t offset) {
        uint32_t value;
        std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    };
    Input in{word(0x60), word(0x64), word(0x0c), word(0x10), word(0x18), word(0x1c),
             bytes[0x14], bytes[0x15], bytes[0x16],
             context.cpuRegs.GPR.r[5].UD[0] != 0, context.cpuRegs.GPR.r[6].UD[0] != 0};
    return Build(in, out);
}
} // namespace title
