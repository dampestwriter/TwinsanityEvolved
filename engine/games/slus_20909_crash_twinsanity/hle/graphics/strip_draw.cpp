#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_draw.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {

struct StripDraw::Rounding {
    int previous = std::fegetround();
    Rounding() { std::fesetround(FE_TOWARDZERO); }
    ~Rounding() { std::fesetround(previous); }
};

namespace {

// TR:draw2d.cpp:15,96,163 at 38d533d0033b48b5d6b49880c49f5eeeade0f38e.
// NTSC: FUN_001a9120 [0x001a9120,0x001a92a0), Write2DVertex 0x001aae28.
// Target dimension halving is arithmetic; offset addition/shift wrap at 32 bits.
float Corner(int32_t size, int32_t offset) {
    const int32_t half = size / 2 - (size < 0 && size % 2 != 0 ? 1 : 0);
    volatile float centered = 2048.0f - static_cast<float>(half);
    const uint32_t shifted =
        (static_cast<uint32_t>(static_cast<int32_t>(centered)) +
         static_cast<uint32_t>(offset)) << 4;
    int32_t signed_corner;
    std::memcpy(&signed_corner, &shifted, sizeof(shifted));
    return static_cast<float>(signed_corner);
}

int32_t Quantize(float position, float corner) {
    volatile float scaled = position * 16.0f;
    volatile float coordinate = scaled + corner;
    // The verified generated cvt.w.s saturates outside the signed word range.
    // Overflow of finite input scaling has the same endpoint after truncation.
    if (coordinate >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
    if (coordinate < -2147483648.0f) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(coordinate);
}

bool OffsetValid(float value) {
    return std::isfinite(value) && value >= 0 && value <= 4095.9375f &&
           std::trunc(value * 16.0f) == value * 16.0f;
}

bool SamePlace(const ps2r::gs::VertexRec& a, const ps2r::gs::VertexRec& b) {
    return a.x == b.x && a.y == b.y;
}

uint32_t Tint(uint32_t color, uint32_t tint) {
    // TR:shapes.cpp:257 and colour.h:55..86. NTSC TintColours 0x1a8f78
    // calls ColourSet{Red,Green,Blue,Alpha} at 0x101748/68/88/28.
    // Separate operations retain the retail 1/192 constant and rounding.
    uint32_t output = 0;
    for (unsigned lane = 0; lane != 4; ++lane) {
        const float reciprocal = lane == 3 ? 0.0078125f : 0x1.555556p-8f;
        const float full = lane == 3 ? 128.0f : 192.0f;
        volatile float tint_fraction = float((tint >> (lane * 8)) & 255) * reciprocal;
        volatile float color_fraction = float((color >> (lane * 8)) & 255) * reciprocal;
        volatile float product = tint_fraction * color_fraction;
        volatile float scaled = product * full;
        const auto byte = static_cast<uint8_t>(static_cast<int32_t>(scaled));
        output |= uint32_t(byte) << (lane * 8);
    }
    return output;
}

}  // namespace

StripDraw::Result StripDraw::Build(const ps2r::gs::DrawIr& state, const Target& target,
                                  const Position* positions, const uint32_t* colors,
                                  size_t count, ps2r::gs::DrawIr& out) {
    return Build(state, target, positions, colors, count, Effects{}, out);
}

StripDraw::Result StripDraw::Build(const ps2r::gs::DrawIr& state, const Target& target,
                                  const Position* positions, const uint32_t* colors,
                                  size_t count, const Effects& effects,
                                  ps2r::gs::DrawIr& out) {
    if (count > kMaxVertices) return Result::InvalidCount;
    if (count && (!positions || !colors)) return Result::MissingInput;
    if (state.tex_enabled || state.prim_type != 4 || state.ctx > 1 ||
        !OffsetValid(state.off_x) || !OffsetValid(state.off_y))
        return Result::UnsupportedState;

    // Geometry is produced on the caller's thread, which need not be an EE
    // thread. Preserve the source's float rounding without leaking it to callers.
    Rounding rounding;

    Matrix matrix{};
    const bool transformed = effects.transform != nullptr;
    const bool tinted = effects.tinted;
    const uint32_t tint = effects.tint;
    if (transformed) {
        std::memcpy(&matrix, effects.transform, sizeof(matrix));
        for (const auto& column : matrix)
            for (float value : column)
                if (!std::isfinite(value)) return Result::InvalidTransform;
    }

    // Source supplies all 32 depth bits; the rendering path converts them on
    // the EE drain thread toward zero. Keep this a runtime conversion so the
    // compiler cannot replace it with a nearest-rounded float literal.
    volatile uint32_t depth_bits = std::numeric_limits<uint32_t>::max();
    const float front_depth = static_cast<float>(depth_bits);
    const float left = Corner(target.display_width, target.offset_x);
    const float top = Corner(target.display_height, target.offset_y);
    std::array<ps2r::gs::VertexRec, kMaxVertices> vertices{};
    for (size_t i = 0; i < count; ++i) {
        Position position;
        uint32_t color;
        std::memcpy(&position, reinterpret_cast<const uint8_t*>(positions) +
                                  i * sizeof(Position), sizeof(position));
        std::memcpy(&color, reinterpret_cast<const uint8_t*>(colors) +
                               i * sizeof(uint32_t), sizeof(color));
        if (!std::isfinite(position.x) || !std::isfinite(position.y))
            return Result::InvalidCoordinates;
        if (transformed) {
            // TR:shapes.cpp:89 constructs z=0/w=1 and keeps only moved.x/y.
            // NTSC 0x1a9448/0x1a9660 call the verified 0x18e4f0 primitive.
            const auto moved = MathLibrary::EvaluateTransformPoint(
                matrix, {position.x, position.y, 0.0f, 1.0f});
            position = {moved.output[0], moved.output[1]};
            if (!std::isfinite(position.x) || !std::isfinite(position.y))
                return Result::InvalidCoordinates;
        }
        if (tinted) color = Tint(color, tint);

        const int32_t x = Quantize(position.x, left);
        const int32_t y = Quantize(position.y, top);
        auto& vertex = vertices[i];
        // Sign extension precedes the OR: X's high half contaminates Y.
        const uint32_t xy = static_cast<uint32_t>(x) | (static_cast<uint32_t>(y) << 16);
        vertex.x = static_cast<float>(xy & 0xffffu) / 16.0f - state.off_x;
        vertex.y = static_cast<float>(xy >> 16) / 16.0f - state.off_y;
        vertex.z = front_depth;
        vertex.q = 1.0f;
        vertex.r = static_cast<uint8_t>(color);
        vertex.g = static_cast<uint8_t>(color >> 8);
        vertex.b = static_cast<uint8_t>(color >> 16);
        vertex.a = static_cast<uint8_t>(color >> 24);
        vertex.draw_kick = i >= 2 ? 1 : 0;
    }

    ps2r::gs::DrawIr result = state;
    result.verts.clear();
    if (count > 2) result.verts.reserve((count - 2) * 3);
    for (size_t newest = 2; newest < count; ++newest) {
        const auto& a = vertices[newest - 2];
        const auto& b = vertices[newest - 1];
        const auto& c = vertices[newest];
        if (SamePlace(a, b) || SamePlace(a, c) || SamePlace(b, c)) continue;
        // Degenerate triangles still consume parity. Keep the newest vertex
        // last because the backend's flat colour comes from that vertex.
        result.verts.push_back(newest % 2 == 0 ? a : b);
        result.verts.push_back(newest % 2 == 0 ? b : a);
        result.verts.push_back(c);
    }
    result.vertex_count = static_cast<uint32_t>(result.verts.size());
    result.snapshot.vertex_hash =
        ps2r::gs::GsdlFormat::VertexHash(result.verts.data(), result.verts.size());
    result.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(result.verts.data(), result.verts.size(), result.bbox);
    out = std::move(result);
    return Result::Ok;
}

}  // namespace title
