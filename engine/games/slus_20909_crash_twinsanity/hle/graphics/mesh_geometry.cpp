#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Vector = MeshGeometry::Vector;
using Matrix = MeshGeometry::Matrix;
using Vertex = MeshGeometry::Vertex;
using Draw = ps2r::gs::DrawIr;
using ScreenVertex = ps2r::gs::VertexRec;

uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}
float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
// Numeric contract of the statically identified type-01 and type-A source programs.
// This is value arithmetic, independent of any hardware register file.
float Finite(float value) {
    const uint32_t bits = Bits(value), exponent = bits & 0x7f800000;
    if (!exponent) return Float(bits & 0x80000000);
    if (exponent == 0x7f800000) return Float((bits & 0x80000000) | 0x7f7fffff);
    return value;
}
float Product(float a, float b) { return Finite(Finite(a) * Finite(b)); }
float Sum(float a, float b) { return Finite(Finite(a) + Finite(b)); }
float MultiplyAdd(float a, float b, float c) {
    return Finite(std::fmaf(Finite(a), Finite(b), Finite(c)));
}
float Reciprocal(float value) {
    value = Finite(value);
    if (value == 0) return Float((Bits(value) & 0x80000000) | 0x7f7fffff);
    return Finite(1.0f / value);
}
uint32_t Integer(float value) {
    value = Finite(value);
    if (value >= 2147483648.0f) return 0x7fffffff;
    if (value < -2147483648.0f) return 0x80000000;
    return uint32_t(int32_t(value));
}
Vector Transform(const Matrix& matrix, const Vector& point, bool strip) {
    Vector result;
    // Strip projection is translation,y,z,x; clip and fan projection are
    // translation,x,y,z. Fusing or reassociating them changes actual vertices.
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
    for (unsigned lane = 0; lane != 4; ++lane) {
        float value = Product(matrix[3][lane], 1);
        for (unsigned row : order) value = MultiplyAdd(matrix[row][lane], point[row], value);
        result[lane] = value;
    }
    return result;
}
uint32_t Outside(const Vector& point) {
    // Compare signed float encodings with |w|, including signed zero and the
    // denormal boundary. This preserves the source's six outside-plane bits.
    const uint32_t w = Bits(point[3]);
    const int32_t bound = int32_t((w & 0x7f800000) ? w & 0x7fffffff : 0x007fffff);
    uint32_t result = 0;
    for (unsigned lane = 0; lane != 3; ++lane) {
        const uint32_t bits = Bits(point[lane]);
        int32_t positive, negative;
        std::memcpy(&positive, &bits, 4);
        const uint32_t flipped = bits ^ 0x80000000;
        std::memcpy(&negative, &flipped, 4);
        if (positive > bound) result |= 1u << (lane * 2);
        if (negative > bound) result |= 2u << (lane * 2);
    }
    return result;
}
bool SamePoint(const ScreenVertex& a, const ScreenVertex& b) { return a.x == b.x && a.y == b.y; }
void Triangle(Draw& draw, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c))
        draw.verts.insert(draw.verts.end(), {a, b, c});
}
ScreenVertex Project(const Vector& position, const Vector& attributes, const Vector& colour,
                     const MeshGeometry::View& view, const Draw& state, bool strip, bool draw) {
    const Vector clip = Transform(view.screen, position, strip);
    const float reciprocal = Reciprocal(clip[3]);
    ScreenVertex result{};
    result.x = float(Integer(Product(clip[0], reciprocal)) & 0xffff) / 16 - state.off_x;
    result.y = float(Integer(Product(clip[1], reciprocal)) & 0xffff) / 16 - state.off_y;
    volatile uint32_t depth = Integer(Product(clip[2], reciprocal));
    result.z = float(depth);
    result.s = Product(Sum(attributes[0], view.scroll[0]), reciprocal);
    result.t = Product(Sum(attributes[1], view.scroll[1]), reciprocal);
    result.q = Product(strip ? Sum(attributes[2], 0) : attributes[2], reciprocal);
    result.r = uint8_t(Integer(colour[0]));
    result.g = uint8_t(Integer(colour[1]));
    result.b = uint8_t(Integer(colour[2]));
    result.a = uint8_t(Integer(colour[3]));
    result.draw_kick = draw;
    return result;
}
struct Corner { Vector position, weight; };
std::vector<Corner> Clip(const std::array<Vertex, 3>& triangle, const Matrix& matrix) {
    std::vector<Corner> polygon;
    for (unsigned i = 0; i != 3; ++i) {
        Corner corner{{triangle[i].position[0], triangle[i].position[1], triangle[i].position[2], 0},
                      {0, 0, 0, 1}};
        corner.weight[i] = 1;
        polygon.push_back(corner);
    }
    for (unsigned plane = 0; plane != 6 && !polygon.empty(); ++plane) {
        Vector row;
        for (unsigned i = 0; i != 4; ++i)
            row[i] = Sum(matrix[i][3], (plane % 2 ? 1 : -1) * Finite(matrix[i][plane / 2]));
        const auto distance = [&](const Corner& corner) {
            float value = Product(row[0], corner.position[0]);
            value = MultiplyAdd(row[1], corner.position[1], value);
            value = MultiplyAdd(row[2], corner.position[2], value);
            return MultiplyAdd(row[3], 1, value);
        };
        std::vector<Corner> next;
        Corner previous = polygon.back();
        float previous_distance = distance(previous);
        for (const auto& current : polygon) {
            const float current_distance = distance(current);
            if (std::signbit(previous_distance) != std::signbit(current_distance)) {
                const float reciprocal = Reciprocal(Finite(current_distance - previous_distance));
                Corner intersection;
                for (unsigned lane = 0; lane != 4; ++lane) {
                    intersection.position[lane] = Product(MultiplyAdd(-current.position[lane],
                        previous_distance, Product(previous.position[lane], current_distance)), reciprocal);
                    intersection.weight[lane] = Product(MultiplyAdd(-current.weight[lane],
                        previous_distance, Product(previous.weight[lane], current_distance)), reciprocal);
                }
                next.push_back(intersection);
            }
            if (!std::signbit(current_distance)) next.push_back(current);
            previous = current;
            previous_distance = current_distance;
        }
        polygon = std::move(next);
    }
    return polygon;
}
void Fan(std::vector<ScreenVertex>& out, const std::array<Vertex, 3>& triangle, const MeshGeometry::View& view,
         const Draw& state) {
    const auto polygon = Clip(triangle, view.clip);
    std::vector<ScreenVertex> projected;
    for (const auto& corner : polygon) {
        Vector stq{}, colour{};
        for (unsigned lane = 0; lane != 4; ++lane) {
            float color = Product(float(triangle[0].colour[lane]), corner.weight[0]);
            color = MultiplyAdd(float(triangle[1].colour[lane]), corner.weight[1], color);
            colour[lane] = MultiplyAdd(float(triangle[2].colour[lane]), corner.weight[2], color);
            if (lane < 3) {
                float attribute = Product(triangle[0].stq[lane], corner.weight[0]);
                attribute = MultiplyAdd(triangle[1].stq[lane], corner.weight[1], attribute);
                stq[lane] = MultiplyAdd(triangle[2].stq[lane], corner.weight[2], attribute);
            }
        }
        projected.push_back(Project(corner.position, stq, colour, view, state, false, true));
    }
    for (size_t i = 2; i < projected.size(); ++i)
        out.insert(out.end(), {projected[0], projected[i - 1], projected[i]});
}
void Fan(Draw& draw, const std::array<Vertex, 3>& triangle, const MeshGeometry::View& view) {
    Fan(draw.verts, triangle, view, draw);
}
void Finish(Draw& draw) {
    draw.vertex_count = uint32_t(draw.verts.size());
    draw.snapshot.vertex_hash = ps2r::gs::GsdlFormat::VertexHash(draw.verts.data(), draw.verts.size());
    draw.snapshot.verts_incl = 1;
    ps2r::gs::GsdlFormat::Bbox(draw.verts.data(), draw.verts.size(), draw.bbox);
}
}  // namespace

MeshGeometry::Result MeshGeometry::Decode(const uint8_t* bytes, size_t size,
                                        uint32_t vertices, Mesh& out, Layout layout) {
    if (!bytes || size < 16 || size > 4 * 1024 * 1024 || vertices < 3)
        return Result::InvalidAsset;
    if (vertices > kMaxVertices) return Result::LimitExceeded;
    size_t cursor = 0, limit = size;
    const auto read = [&](uint32_t& value) {
        if (cursor > limit || limit - cursor < 4) return false;
        value = uint32_t(bytes[cursor]) | uint32_t(bytes[cursor + 1]) << 8 |
                uint32_t(bytes[cursor + 2]) << 16 | uint32_t(bytes[cursor + 3]) << 24;
        cursor += 4;
        return true;
    };
    const auto expect = [&](uint32_t value) { uint32_t actual; return read(actual) && actual == value; };
    uint32_t header;
    if (!read(header) || (header >> 16) != 0x6000 || !expect(0) || !expect(0x070000fa) || !expect(0))
        return Result::InvalidAsset;
    limit = 16 + size_t(header & 0xffff) * 16;
    if (limit > size || size - limit > 36) return Result::InvalidAsset;
    for (size_t i = limit; i < size; ++i) if (bytes[i]) return Result::InvalidAsset;
    Mesh mesh;
    uint32_t total = 0;
    while (cursor < limit) {
        uint32_t command;
        if (!read(command)) return Result::InvalidAsset;
        if (command == 0) {
            for (size_t i = cursor; i < limit; ++i) if (bytes[i]) return Result::InvalidAsset;
            break;
        }
        if (command != 0x6c018000) return Result::InvalidAsset;
        uint32_t count;
        if (!read(count) || (count & ~0xffu) != 0x8000) return Result::InvalidAsset;
        count &= 255;
        if (count < 3 || count > 38 || total + count > vertices) return Result::InvalidAsset;
        if (!expect(0x30024000) || !expect(0x512) || !expect(0) ||
            !expect(0x01000101) || !expect(0x64018001) || !expect(count * 4) ||
            !expect(count | 0x8000) || !expect(0x01000104) ||
            !expect(0x68008003 | count << 16)) return Result::InvalidAsset;
        Strip strip;
        strip.vertices.resize(count);
        for (auto& vertex : strip.vertices)
            for (float& coordinate : vertex.position) {
                uint32_t bits;
                if (!read(bits) || !std::isfinite(coordinate = Float(bits))) return Result::InvalidAsset;
            }
        if (!expect(0x6c008004 | count << 16)) return Result::InvalidAsset;
        for (auto& vertex : strip.vertices)
            for (unsigned lane = 0; lane != 4; ++lane) {
                uint32_t bits;
                if (!read(bits)) return Result::InvalidAsset;
                vertex.colour[lane] = uint8_t(bits);
                if (lane < 3) {
                    if (!std::isfinite(vertex.stq[lane] = Float(bits))) return Result::InvalidAsset;
                } else vertex.restart = bits & 0x8000;
            }
        uint32_t end;
        if (!read(end)) return Result::InvalidAsset;
        if (layout == Layout::Unlit) {
            // Shared authored models can carry normals and a separate packed
            // colour stream for other materials. Type01 never reads those
            // fields; its colour is the low byte of each STQ/ADC word above.
            // Accept only these two enumerated record shapes, once/in order.
            for (const auto& optional : std::array<std::array<uint32_t, 2>, 2>{{
                     {0x68008005u | count << 16, count * 3},
                     {0x6e00c006u | count << 16, count}}}) {
                if (end != optional[0]) continue;
                const size_t bytes = size_t(optional[1]) * 4;
                if (cursor > limit || bytes > limit - cursor) return Result::InvalidAsset;
                cursor += bytes;
                if (!read(end)) return Result::InvalidAsset;
            }
        }
        if (end != 0x14000000 || !expect(0x01000101)) return Result::InvalidAsset;
        total += count;
        mesh.strips.push_back(std::move(strip));
    }
    if (total != vertices) return Result::InvalidAsset;
    out = std::move(mesh);
    return Result::Ok;
}

MeshGeometry::Result MeshGeometry::Build(const Mesh& mesh, const std::vector<View>& instances, const Draw& state,
                                       std::vector<Draw>& out) {
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog ||
        state.raster.frame.psm || state.raster.zbuf.psm || state.prim_type != 4 ||
        !std::isfinite(state.off_x) || !std::isfinite(state.off_y))
        return Result::UnsupportedState;
    if (instances.empty() || instances.size() > 20) return Result::InvalidGeometry;
    for (const auto& view : instances) {
    for (const auto* matrix : {&view.screen, &view.clip})
        for (const auto& row : *matrix)
            for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.guard) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.scroll) if (!std::isfinite(value)) return Result::InvalidGeometry;
    }
    RoundingGuard rounding;
    size_t count = 0, output_count = 0;
    std::vector<Draw> result;
    for (const auto& strip : mesh.strips) {
        const size_t size = strip.vertices.size();
        count += size;
        if (size < 3 || size > 38) return Result::InvalidGeometry;
        if (count > kMaxVertices) return Result::LimitExceeded;
        for (const auto& view : instances) {
        Draw body = state;
        body.verts.clear();
        const auto publish = [&](Draw& draw) {
            output_count += draw.verts.size();
            if (output_count > kMaxOutputVertices) return false;
            if (!draw.verts.empty()) {
                Finish(draw);
                result.push_back(std::move(draw));
            }
            return true;
        };
        std::array<uint32_t, 3> hard{}, guard{};
        std::array<ScreenVertex, 38> projected{};
        unsigned parity = 0;
        for (size_t i = 0; i < size; ++i) {
            const auto& vertex = strip.vertices[i];
            Vector point{vertex.position[0], vertex.position[1], vertex.position[2], 1};
            for (float value : point) if (!std::isfinite(value)) return Result::InvalidGeometry;
            for (float value : vertex.stq) if (!std::isfinite(value)) return Result::InvalidGeometry;
            Vector clipped = Transform(view.clip, point, false), guarded;
            for (unsigned lane = 0; lane != 4; ++lane) guarded[lane] = Product(clipped[lane], view.guard[lane]);
            hard[i % 3] = Outside(clipped);
            guard[i % 3] = Outside(guarded);
            bool emit = !vertex.restart;
            if (view.clipped && i >= 2) {
                if (hard[0] & hard[1] & hard[2]) emit = false;
                else if (guard[0] | guard[1] | guard[2]) {
                    if (emit) {
                        Draw fan = state;
                        fan.prim_type = 5;
                        fan.verts.clear();
                        Fan(fan, {strip.vertices[i - 2], strip.vertices[i - 1], vertex}, view);
                        if (!publish(fan)) return Result::LimitExceeded;
                    }
                    emit = false;
                }
            }
            projected[i] = Project(point, {vertex.stq[0], vertex.stq[1], vertex.stq[2], 0},
                {float(vertex.colour[0]), float(vertex.colour[1]), float(vertex.colour[2]),
                 float(vertex.colour[3])}, view, body, true, emit);
            if (!emit) parity = 0;
            else if (i >= 2) {
                Triangle(body, projected[i - 2 + (parity & 1)], projected[i - 1 - (parity & 1)], projected[i]);
                ++parity;
            }
        }
        if (!publish(body)) return Result::LimitExceeded;
        }
    }
    out = std::move(result);
    return Result::Ok;
}

// ---- prepared builds -----------------------------------------------------------------------------------------
// Every operation below is one of Build's, on operands Build would have flushed on the way in (Finite is
// idempotent, Finite(x) * 1 == Finite(x), so flushing the mesh and the matrices once and then applying the
// same multiply-adds in the same order yields the same bits). The lane-parallel transform is the same fused
// multiply-add per lane (the AArch64 SIMD unit rounds by FPCR like the scalar one; the rounding guard
// covers both) with the flush as integer selects.
namespace {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define TITLE_MESH_NEON 1
#else
#define TITLE_MESH_NEON 0
#endif

inline Vector FlushVector(const Vector& v) { return {Finite(v[0]), Finite(v[1]), Finite(v[2]), Finite(v[3])}; }
inline Matrix FlushMatrix(const Matrix& m) {
    return {FlushVector(m[0]), FlushVector(m[1]), FlushVector(m[2]), FlushVector(m[3])};
}

#if TITLE_MESH_NEON
inline float32x4_t FiniteLanes(float32x4_t value) {
    const uint32x4_t bits = vreinterpretq_u32_f32(value);
    const uint32x4_t exponent = vandq_u32(bits, vdupq_n_u32(0x7f800000u));
    const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(0x80000000u));
    const uint32x4_t zero_exponent = vceqq_u32(exponent, vdupq_n_u32(0));
    const uint32x4_t full_exponent = vceqq_u32(exponent, vdupq_n_u32(0x7f800000u));
    const uint32x4_t saturated = vorrq_u32(sign, vdupq_n_u32(0x7f7fffffu));
    const uint32x4_t result = vbslq_u32(zero_exponent, sign, vbslq_u32(full_exponent, saturated, bits));
    return vreinterpretq_f32_u32(result);
}
#endif

// Transform on flushed operands: value = Finite(m[3]); for row in order: value = Finite(fma(m[row], p[row], value))
inline Vector TransformFlushed(const Matrix& matrix, const Vector& point, bool strip) {
    const std::array<unsigned, 3> order = strip ? std::array<unsigned, 3>{1, 2, 0}
                                               : std::array<unsigned, 3>{0, 1, 2};
#if TITLE_MESH_NEON
    float32x4_t value = vld1q_f32(matrix[3].data());
    for (unsigned row : order)
        value = FiniteLanes(vfmaq_n_f32(value, vld1q_f32(matrix[row].data()), point[row]));
    Vector result;
    vst1q_f32(result.data(), value);
    return result;
#else
    Vector result;
    for (unsigned lane = 0; lane != 4; ++lane) {
        float value = matrix[3][lane];
        for (unsigned row : order) value = Finite(std::fmaf(matrix[row][lane], point[row], value));
        result[lane] = value;
    }
    return result;
#endif
}

// Project on a transformed (flushed) point: Build's Project with the colour bytes passed through (Integer(float(b)) == b)
inline ScreenVertex ProjectFlushed(const Vector& clip, const Vector& stq, const std::array<uint8_t, 4>& colour,
                                   const Vector& scroll, const Draw& state, bool draw) {
    const float reciprocal = Reciprocal(clip[3]);
    ScreenVertex result{};
    result.x = float(Integer(Finite(clip[0] * reciprocal)) & 0xffff) / 16 - state.off_x;
    result.y = float(Integer(Finite(clip[1] * reciprocal)) & 0xffff) / 16 - state.off_y;
    volatile uint32_t depth = Integer(Finite(clip[2] * reciprocal));
    result.z = float(depth);
    result.s = Finite(Finite(stq[0] + scroll[0]) * reciprocal);
    result.t = Finite(Finite(stq[1] + scroll[1]) * reciprocal);
    result.q = Finite(Finite(stq[2] + 0.0f) * reciprocal);
    result.r = colour[0];
    result.g = colour[1];
    result.b = colour[2];
    result.a = colour[3];
    result.draw_kick = draw;
    return result;
}
}  // namespace

MeshGeometry::Result MeshGeometry::Prepare(const Mesh& mesh, Prepared& out) {
    Prepared result;
    size_t count = 0;
    for (const auto& strip : mesh.strips) {
        const size_t size = strip.vertices.size();
        count += size;
        if (size < 3 || size > 38) return Result::InvalidGeometry;
        if (count > kMaxVertices) return Result::LimitExceeded;
        for (const auto& vertex : strip.vertices) {
            for (float value : vertex.position) if (!std::isfinite(value)) return Result::InvalidGeometry;
            for (float value : vertex.stq) if (!std::isfinite(value)) return Result::InvalidGeometry;
        }
    }
    result.strips.reserve(mesh.strips.size());
    result.positions.reserve(count);
    result.stq.reserve(count);
    result.colours.reserve(count);
    result.restart.reserve(count);
    for (const auto& strip : mesh.strips) {
        result.strips.push_back({uint32_t(result.positions.size()), uint32_t(strip.vertices.size())});
        for (const auto& vertex : strip.vertices) {
            result.positions.push_back(FlushVector({vertex.position[0], vertex.position[1], vertex.position[2], 1}));
            result.stq.push_back(FlushVector({vertex.stq[0], vertex.stq[1], vertex.stq[2], 0}));
            result.colours.push_back(vertex.colour);
            result.restart.push_back(vertex.restart ? 1 : 0);
        }
    }
    result.vertex_count = count;
    out = std::move(result);
    return Result::Ok;
}

MeshGeometry::Result MeshGeometry::BuildPrepared(const Prepared& prepared, const View& view, const Draw& state,
                                                const std::array<uint8_t, 4>* colours, Sink sink, void* user) {
    if (!state.tex_enabled || state.use_fst || state.tex.fst || state.ctx || state.fog ||
        state.raster.frame.psm || state.raster.zbuf.psm || state.prim_type != 4 ||
        !std::isfinite(state.off_x) || !std::isfinite(state.off_y))
        return Result::UnsupportedState;
    for (const auto* matrix : {&view.screen, &view.clip})
        for (const auto& row : *matrix)
            for (float value : row) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.guard) if (!std::isfinite(value)) return Result::InvalidGeometry;
    for (float value : view.scroll) if (!std::isfinite(value)) return Result::InvalidGeometry;
    if (!sink) return Result::InvalidGeometry;
    RoundingGuard rounding;
    const Matrix screen = FlushMatrix(view.screen), clip = FlushMatrix(view.clip);
    const Vector guard = FlushVector(view.guard);
    const Vector scroll{Finite(view.scroll[0]), Finite(view.scroll[1]), 0, 0};
    std::array<ScreenVertex, 38 * 3> body;
    std::vector<ScreenVertex> fan;
    for (const auto& range : prepared.strips) {
        const size_t first = range.first, size = range.count;
        size_t body_count = 0;
        std::array<uint32_t, 3> hard{}, guarded{};
        std::array<ScreenVertex, 38> projected;
        unsigned parity = 0;
        for (size_t i = 0; i < size; ++i) {
            const size_t at = first + i;
            const Vector& point = prepared.positions[at];
            const std::array<uint8_t, 4>& colour = colours ? colours[at] : prepared.colours[at];
            bool emit = !prepared.restart[at];
            if (view.clipped) {
                const Vector clipped = TransformFlushed(clip, point, false);
                Vector banded;
                for (unsigned lane = 0; lane != 4; ++lane) banded[lane] = Finite(clipped[lane] * guard[lane]);
                hard[i % 3] = Outside(clipped);
                guarded[i % 3] = Outside(banded);
                if (i >= 2) {
                    if (hard[0] & hard[1] & hard[2]) emit = false;
                    else if (guarded[0] | guarded[1] | guarded[2]) {
                        if (emit) {
                            // The clipped triangle's fan, from the mesh's vertices (the flushed values are what
                            // Clip's and Project's own flushes would have made of the originals)
                            std::array<Vertex, 3> triangle;
                            for (unsigned k = 0; k != 3; ++k) {
                                const size_t v = at - 2 + k;
                                triangle[k].position = {prepared.positions[v][0], prepared.positions[v][1], prepared.positions[v][2]};
                                triangle[k].stq = {prepared.stq[v][0], prepared.stq[v][1], prepared.stq[v][2]};
                                triangle[k].colour = colours ? colours[v] : prepared.colours[v];
                                triangle[k].restart = prepared.restart[v] != 0;
                            }
                            fan.clear();
                            Fan(fan, triangle, view, state);
                            if (!fan.empty()) sink(user, fan.data(), fan.size(), true);
                        }
                        emit = false;
                    }
                }
            }
            projected[i] = ProjectFlushed(TransformFlushed(screen, point, true), prepared.stq[at], colour, scroll, state, emit);
            if (!emit) parity = 0;
            else if (i >= 2) {
                const ScreenVertex& a = projected[i - 2 + (parity & 1)];
                const ScreenVertex& b = projected[i - 1 - (parity & 1)];
                const ScreenVertex& c = projected[i];
                if (!SamePoint(a, b) && !SamePoint(b, c) && !SamePoint(a, c)) {
                    body[body_count++] = a;
                    body[body_count++] = b;
                    body[body_count++] = c;
                }
                ++parity;
            }
        }
        if (body_count) sink(user, body.data(), body_count, false);
    }
    return Result::Ok;
}

}  // namespace title
