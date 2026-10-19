#include "games/slus_20909_crash_twinsanity/hle/graphics/primitives/sprite_draw.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "engine/ee/cpu/ps2_float.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
struct SpriteDraw::Rounding {
    int previous = std::fegetround();
    Rounding() { std::fesetround(FE_TOWARDZERO); }
    ~Rounding() { std::fesetround(previous); }
};
namespace {
// Reuse the same pure scalar policy as MathLibrary. Each source operation
// rounds separately; no instruction interpreter or processor owner is used.
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
int32_t Integer(float value) {
    if (value >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
    if (value < -2147483648.0f) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}
float Corner(int32_t extent, int32_t offset) {
    const int32_t half = extent / 2 - (extent < 0 && extent % 2 ? 1 : 0);
    const uint32_t bits = (uint32_t(Integer(Sub(2048.0f, float(half)))) + uint32_t(offset)) << 4;
    int32_t value;
    std::memcpy(&value, &bits, sizeof(value));
    return float(value);
}
bool Offset(float x) {
    return std::isfinite(x) && x >= 0 && x <= 4095.9375f && std::trunc(x * 16) == x * 16;
}
bool Same(const ps2r::gs::VertexRec& a, const ps2r::gs::VertexRec& b) { return a.x == b.x && a.y == b.y; }
}
SpriteDraw::Result SpriteDraw::Build(const ps2r::gs::DrawIr& state, const Target& target,
                                    const Input& input, ps2r::gs::DrawIr& out) {
    if (state.prim_type != 4 || state.ctx > 1 || state.fog ||
        (state.tex_enabled && (state.use_fst || state.tex.fst)) ||
        !Offset(state.off_x) || !Offset(state.off_y)) return Result::InvalidState;
    if (target.screen_width <= 0 || target.screen_width > 4096 ||
        target.screen_height <= 0 || target.screen_height > 4096 ||
        !std::isfinite(input.texture.x) || !std::isfinite(input.texture.y) ||
        !std::isfinite(input.texture.width) || !std::isfinite(input.texture.height))
        return Result::InvalidInput;
    Matrix matrix{};
    if (input.matrix) {
        matrix = *input.matrix;
        for (const auto& column : matrix)
            for (float value : column)
                if (!std::isfinite(value)) return Result::InvalidInput;
    }
    Rounding rounding;
    const auto& c = target.coordinates;
    const bool turned = input.matrix && input.turned;
    const float corner_x = Corner(c.display_width, turned ? 0 : c.offset_x);
    const float corner_y = Corner(c.display_height, turned ? 0 : c.offset_y);
    std::array<std::array<float, 2>, 4> positions{};
    if (turned) {
        // Turned corners use a screen-size*16 product before point scaling.
        const float sx = Mul(float(target.screen_width), 16), sy = Mul(float(target.screen_height), 16);
        constexpr std::array<MathLibrary::Vector, 4> corners{{
            {-0.5f, 0.5f, 0, 1}, {-0.5f, -0.5f, 0, 1},
            {0.5f, 0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}}};
        for (size_t i = 0; i < 4; ++i) {
            const auto p = MathLibrary::EvaluateTransformPoint(matrix, corners[i]).output;
            positions[i] = {Add(Mul(p[0], sx), corner_x), Add(Mul(p[1], sy), corner_y)};
        }
    } else {
        MathLibrary::Vector a{0, 1, 0, 1}, b{1, 0, 0, 1};
        if (input.matrix) {
            a = MathLibrary::EvaluateTransformPoint(matrix, a).output;
            b = MathLibrary::EvaluateTransformPoint(matrix, b).output;
        }
        const float left = Add(Mul(Mul(a[0], float(target.screen_width)), 16), corner_x);
        const float top = Add(Mul(Mul(a[1], float(target.screen_height)), 16), corner_y);
        const float right = Add(left, Mul(Mul(Sub(b[0], a[0]), float(target.screen_width)), 16));
        const float bottom = Add(top, Mul(Mul(Sub(b[1], a[1]), float(target.screen_height)), 16));
        positions = {{{left, top}, {left, bottom}, {right, top}, {right, bottom}}};
    }
    const auto area = input.texture;
    const std::array<std::array<float, 2>, 4> coordinates{{
        {area.x, area.y}, {area.x, Add(area.y, area.height)},
        {Add(area.x, area.width), area.y}, {Add(area.x, area.width), Add(area.y, area.height)}}};
    std::array<ps2r::gs::VertexRec, 4> vertices{};
    volatile uint32_t depth_bits = 0xffffffff;
    const float depth = float(depth_bits);
    for (size_t i = 0; i < 4; ++i) {
        const auto& p = positions[i];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1])) return Result::InvalidInput;
        const uint32_t xy = uint32_t(Integer(p[0])) | (uint32_t(Integer(p[1])) << 16);
        auto& v = vertices[i];
        v.x = float(xy & 0xffff) / 16 - state.off_x;
        v.y = float(xy >> 16) / 16 - state.off_y;
        v.z = depth;
        v.s = coordinates[i][0]; v.t = coordinates[i][1]; v.q = 1;
        v.r = uint8_t(input.colour); v.g = uint8_t(input.colour >> 8);
        v.b = uint8_t(input.colour >> 16); v.a = uint8_t(input.colour >> 24);
        v.draw_kick = i >= 2;
    }
    auto result = state;
    result.verts.clear();
    for (size_t i = 2; i < 4; ++i) {
        const auto& a = vertices[i - 2]; const auto& b = vertices[i - 1]; const auto& c = vertices[i];
        if (Same(a, b) || Same(a, c) || Same(b, c)) continue;
        result.verts.push_back(i == 2 ? a : b);
        result.verts.push_back(i == 2 ? b : a);
        result.verts.push_back(c);
    }
    result.vertex_count = uint32_t(result.verts.size());
    result.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(result.verts.data(), result.verts.size());
    result.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(result.verts.data(), result.verts.size(), result.bbox);
    out = std::move(result);
    return Result::Ok;
}
}  // namespace title
