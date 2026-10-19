#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/wave_geometry.h"

#include <cmath>
#include <cstring>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float Float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    if (exponent == 0x7f800000) return Float((bits & 0x80000000) | 0x7f7fffff);
    return value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
uint32_t Integer(float value) {
    value = Finite(value);
    if (value >= 2147483648.0f) return 0x7fffffff;
    if (value < -2147483648.0f) return 0x80000000;
    return uint32_t(int32_t(value));
}
bool Dimensions(uint32_t width, uint32_t height) {
    return width && width <= 2048 && height >= 32 && height <= 2048;
}
ps2r::gs::DrawIr State(uint32_t width, uint32_t height, uint32_t frame) {
    ps2r::gs::DrawIr draw;
    draw.src_frame = frame;
    draw.prim_type = 4;
    draw.use_fst = draw.tex_enabled = draw.alpha_blend = true;
    (void)width;
    (void)height;
    draw.raster.frame.fbw = 512;
    draw.raster.frame.fbmsk = 0xff000000;
    draw.raster.zbuf.zmsk = true;
    draw.raster.test.zte = true;
    draw.raster.test.ztst = 3;
    draw.raster.colclamp = true;
    draw.raster.scissor = {0, 0, 1024, 1024};
    draw.raster.alpha = {1, 0, 1, 0, 32};
    draw.tex.fst = true;
    draw.tex.tcc = draw.tex.tfx = 1;
    draw.tex.cld = 1;
    draw.tex.tbw = 512;
    draw.tex.th = 10;
    draw.tex.tw = draw.tex.th;
    draw.tex.texa_ta1 = 128;
    draw.tex.mmin = 1;
    draw.tex.mmag = draw.tex.mmin;
    return draw;
}
ps2r::gs::VertexRec Project(const WaveGeometry::Vertex& vertex,
                            const WaveGeometry::Waves& waves, uint32_t width, uint32_t height) {
    ps2r::gs::VertexRec result{};
    float* screen[] = {&result.x, &result.y};
    const uint32_t offsets[] = {2048 - (width >> 1), 2048 - (height >> 1)};
    for (size_t axis = 0; axis < 2; ++axis) {
        // Static type-1C 0x01c..0x027: MTIR of the FLOAT PRODUCT's low
        // halfword, masked by 15. Converting the product to integer first is
        // a different lookup and loses the source deformation.
        const uint32_t row = Bits(Product(vertex.position[axis], Float(0x39c38f36))) & 15;
        const float offset = Product(waves.rows[row][axis], waves.amplitude);
        const float position = Finite(std::fmaf(Finite(vertex.position[axis]), 1, offset));
        *screen[axis] = float(Integer(Product(position, 16)) & 0xffff) / 16 - float(offsets[axis]);
    }
    result.z = float(vertex.depth);
    result.uv_u = vertex.texture[0] & 0x3fff;
    result.uv_v = vertex.texture[1] & 0x3fff;
    result.s = float(result.uv_u) / 16;
    result.t = float(result.uv_v) / 16;
    result.q = 1;
    result.r = result.g = result.b = result.a = 127;
    result.draw_kick = !vertex.restart;
    return result;
}
bool EqualPosition(const ps2r::gs::VertexRec& a, const ps2r::gs::VertexRec& b) {
    return a.x == b.x && a.y == b.y;
}
}  // namespace

WaveGeometry::Result WaveGeometry::MakeGrid(uint32_t width, uint32_t height, Grid& out) {
    if (!Dimensions(width, height)) return Result::InvalidDimensions;
    Grid grid{width, height, {}};
    grid.strips.reserve(64);
    const uint32_t left = 2048 - (width >> 1), top = 2048 - (height >> 1), step = height >> 5;
    for (unsigned half = 0; half < 2; ++half)
        for (unsigned column = 0; column < 32; ++column) {
            Strip strip;
            const unsigned first = half ? 15 : 0, rows = half ? 17 : 16;
            for (unsigned row = 0; row < rows; ++row)
                for (unsigned side = 0; side < 2; ++side) {
                    const uint32_t x = (column + side) * 16, y = (first + row) * step;
                    strip.vertices.push_back({{float(left + x), float(top + y)},
                        {uint16_t(x * 16 + 8), uint16_t(y * 16 + 8)}, 0x7000, row == 0});
                }
            grid.strips.push_back(std::move(strip));
        }
    out = std::move(grid);
    return Result::Ok;
}

WaveGeometry::Result WaveGeometry::Build(const Grid& grid, const Waves& waves, uint32_t frame,
                                        std::vector<ps2r::gs::DrawIr>& out) {
    if (!Dimensions(grid.width, grid.height)) return Result::InvalidDimensions;
    if (!std::isfinite(waves.amplitude)) return Result::InvalidWaves;
    for (const auto& row : waves.rows)
        for (float value : row) if (!std::isfinite(value)) return Result::InvalidWaves;
    if (grid.strips.size() > 64) return Result::LimitExceeded;
    for (const auto& strip : grid.strips) {
        if (strip.vertices.size() < 3 || strip.vertices.size() > 38) return Result::InvalidGeometry;
        for (const auto& vertex : strip.vertices)
            for (float value : vertex.position) if (!std::isfinite(value)) return Result::InvalidGeometry;
    }
    RoundingGuard rounding;
    std::vector<ps2r::gs::DrawIr> output;
    for (const auto& strip : grid.strips) {
        auto draw = State(grid.width, grid.height, frame);
        std::array<ps2r::gs::VertexRec, 3> previous{};
        unsigned parity = 0;
        for (size_t i = 0; i < strip.vertices.size(); ++i) {
            previous[i % 3] = Project(strip.vertices[i], waves, grid.width, grid.height);
            if (strip.vertices[i].restart) parity = 0;
            else if (i >= 2) {
                const auto& a = previous[(i - 2 + (parity & 1)) % 3];
                const auto& b = previous[(i - 1 - (parity & 1)) % 3];
                const auto& c = previous[i % 3];
                if (!EqualPosition(a, b) && !EqualPosition(b, c) && !EqualPosition(a, c))
                    draw.verts.insert(draw.verts.end(), {a, b, c});
                ++parity;
            }
        }
        if (draw.verts.empty()) continue;
        draw.vertex_count = uint32_t(draw.verts.size());
        draw.snapshot.verts_incl = 1;
        draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
        ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
        output.push_back(std::move(draw));
    }
    out = std::move(output);
    return Result::Ok;
}
}  // namespace title
