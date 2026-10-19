#include "games/slus_20909_crash_twinsanity/hle/graphics/text/text_draw.h"

#include <cfenv>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/ee/cpu/ps2_float.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
struct TextDraw::Rounding {
    int previous = std::fegetround();
    Rounding() { std::fesetround(FE_TOWARDZERO); }
    ~Rounding() { std::fesetround(previous); }
};
namespace {
float Add(float a, float b) {
    volatile float value = ps2_float_clamp(a) + ps2_float_clamp(b);
    return ps2_float_clamp(value);
}
float Sub(float a, float b) {
    volatile float value = ps2_float_clamp(a) - ps2_float_clamp(b);
    return ps2_float_clamp(value);
}
float Mul(float a, float b) {
    volatile float value = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(value);
}
uint32_t Integer(float value) {
    value = ps2_float_clamp(value);
    if (value >= 2147483648.0f) return uint32_t(std::numeric_limits<int32_t>::max());
    if (value < -2147483648.0f) return uint32_t(std::numeric_limits<int32_t>::min());
    return uint32_t(static_cast<int32_t>(value));
}
ps2r::gs::VertexRec Vertex(float x, float y, float u, float v, uint32_t colour,
                          const ps2r::gs::DrawIr& state) {
    ps2r::gs::VertexRec out{};
    out.x = float(Integer(x) & 0xffff) / 16 - state.off_x;
    out.y = float(Integer(y) & 0xffff) / 16 - state.off_y;
    out.uv_u = uint16_t(Integer(u) & 0x3fff); out.uv_v = uint16_t(Integer(v) & 0x3fff);
    out.s = float(out.uv_u) / 16; out.t = float(out.uv_v) / 16; out.q = 1; out.z = 1;
    out.r = uint8_t(colour); out.g = uint8_t(colour >> 8);
    out.b = uint8_t(colour >> 16); out.a = uint8_t(colour >> 24); out.draw_kick = 1;
    return out;
}
void Finish(ps2r::gs::DrawIr& draw, std::vector<ps2r::gs::DrawIr>& output) {
    if (draw.verts.empty()) return;
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    draw.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
    output.push_back(draw);
    draw.verts.clear();
}
}
TextDraw::Result TextDraw::Build(const ps2r::gs::DrawIr& state, const TextLayout::Font& font,
                                 const TextLayout::Output& layout, uint8_t page,
                                 int32_t frame_width, int32_t frame_height,
                                 std::vector<ps2r::gs::DrawIr>& out) {
    if (state.prim_type != 6 || !state.use_fst || !state.tex.fst || !state.tex_enabled ||
        state.ctx != 0 || state.fog || !state.verts.empty() ||
        !std::isfinite(state.off_x) || !std::isfinite(state.off_y)) return Result::InvalidState;
    if (page < 1 || page > 3 || frame_width <= 0 || frame_width > 4096 ||
        frame_height <= 0 || frame_height > 4096 || !std::isfinite(layout.scale_x) ||
        !std::isfinite(layout.scale_y) || layout.lines.size() > TextLayout::kMaxLines)
        return Result::InvalidInput;
    Rounding rounding;
    const float corner_x = Sub(32768, Mul(float(frame_width * 16), .5f));
    const float corner_y = Sub(32768, Mul(float(frame_height * 16), .5f));
    std::vector<ps2r::gs::DrawIr> result;
    auto draw = state;
    size_t next_line = 0;
    for (const auto& group : layout.groups) {
        if (!group.count || group.first != next_line || group.first > layout.lines.size() ||
            group.count > layout.lines.size() - group.first) return Result::InvalidInput;
        for (size_t n = 0; n < group.count; ++n) {
            const auto& line = layout.lines[group.first + n];
            if (!std::isfinite(line.x) || !std::isfinite(line.y) ||
                line.glyphs.empty() || (line.glyphs.size() & 3) ||
                line.glyphs.size() > 0x260) return Result::InvalidInput;
            float cursor = Add(line.x, corner_x);
            const float y = Add(line.y, corner_y);
            for (uint8_t index : line.glyphs) {
                if (index >= font.glyphs.size()) return Result::InvalidInput;
                const auto& glyph = font.glyphs[index];
                if (!std::isfinite(glyph.u) || !std::isfinite(glyph.v) ||
                    !std::isfinite(glyph.width) || !std::isfinite(glyph.height)) return Result::InvalidInput;
                const float right = Add(cursor, Mul(glyph.width, layout.scale_x));
                const float bottom = Add(y, Mul(glyph.height, layout.scale_y));
                if (TextLayout::Page(glyph) == page) {
                    auto a = Vertex(cursor, y, glyph.u, glyph.v, layout.colour, state);
                    auto b = Vertex(right, bottom, Add(glyph.u, glyph.width),
                                    Sub(glyph.v, glyph.height), layout.colour, state);
                    // Canonical rectangle order includes reflected scales and
                    // corners wrapping across the source coordinate boundary.
                    if (a.x > b.x) {
                        std::swap(a.x, b.x); std::swap(a.s, b.s); std::swap(a.uv_u, b.uv_u);
                    }
                    if (a.y > b.y) {
                        std::swap(a.y, b.y); std::swap(a.t, b.t); std::swap(a.uv_v, b.uv_v);
                    }
                    auto top_right = a, bottom_left = b;
                    top_right.x = b.x; top_right.s = b.s; top_right.uv_u = b.uv_u;
                    bottom_left.x = a.x; bottom_left.s = a.s; bottom_left.uv_u = a.uv_u;
                    draw.verts.insert(draw.verts.end(), {a, top_right, bottom_left, top_right, b, bottom_left});
                    if (draw.verts.size() == 20 * 6) Finish(draw, result);
                }
                cursor = right;
            }
        }
        Finish(draw, result);
        next_line += group.count;
    }
    if (next_line != layout.lines.size()) return Result::InvalidInput;
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
