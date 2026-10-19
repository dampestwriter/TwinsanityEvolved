#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_draw.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace {

using Strip = title::StripDraw;
using Draw = ps2r::gs::DrawIr;
using Vertex = ps2r::gs::VertexRec;
using Result = Strip::Result;
int checks = 0, failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

uint32_t Bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

uint32_t Color(const Vertex& v) {
    return uint32_t(v.r) | (uint32_t(v.g) << 8) |
           (uint32_t(v.b) << 16) | (uint32_t(v.a) << 24);
}

Draw State() {
    Draw d;
    d.prim_type = 4;
    d.seq = 17; d.src_frame = 923; d.vram_epoch = 0x123456789abcdef0ull;
    d.identity = {42, 0x918273645a6b7c8dull, 89, 123, 456};
    d.ctx = 1;
    d.gouraud = true; d.fog = true; d.use_fst = true; d.alpha_blend = true;
    d.scanmsk = 2; d.shuffle_type = 3; d.shuffle_uv_scale_u = 0.25f;
    d.raster.frame = {0x100000, 640, 2, 0x81735afcu};
    d.raster.zbuf = {0x180000, 0x32, true};
    d.raster.test = {true, 5, 0x6d, 2, true, true, true, 3};
    d.raster.alpha = {2, 1, 2, 0, 0x74};
    d.raster.scissor = {9, 7, 632, 469};
    d.raster.colclamp = true; d.raster.pabe = true; d.raster.fba = true;
    d.raster.fogcol_r = 31; d.raster.fogcol_g = 73; d.raster.fogcol_b = 151;
    d.raster.dthe = 1; d.raster.dimx = 0x1023456789abcdefull;
    // Inactive texture state is still caller-owned state and must survive.
    d.tex.tbp = 0x190000; d.tex.tbw = 256; d.tex.psm = 19;
    d.tex.tw = 7; d.tex.th = 8; d.tex.tcc = 1; d.tex.tfx = 2;
    d.tex.cbp = 0x270000; d.tex.cpsm = 2; d.tex.csm = 1;
    d.tex.csa = 17; d.tex.cld = 4; d.tex.fst = true;
    d.tex.mmag = true; d.tex.mmin = 5; d.tex.mxl = 3;
    d.tex.lcm = true; d.tex.l = 2; d.tex.k = -113;
    d.tex.wms = 2; d.tex.wmt = 3;
    d.tex.minu = 1; d.tex.maxu = 31; d.tex.minv = 3; d.tex.maxv = 79;
    d.tex.texa_ta0 = 3; d.tex.texa_ta1 = 127; d.tex.texa_aem = true;
    for (size_t i = 0; i < 6; ++i) {
        d.tex.mip_tbp[i] = uint32_t(0x280000 + i * 0x1000);
        d.tex.mip_tbw[i] = uint32_t(64 * (i + 1));
    }
    d.snapshot = {111, 222, 333, 444, 555, 0};
    d.verts.resize(9);
    d.vertex_count = 999;  // stale geometry must not leak into the result
    for (float& value : d.bbox) value = -666;
    return d;
}

void CheckState(const Draw& actual, const Draw& expected) {
    auto a = actual.ToDrawRec(), b = expected.ToDrawRec();
    a.vertex_count = b.vertex_count = 0;
    a.vertex_hash = b.vertex_hash = 0;
    a.verts_incl = b.verts_incl = 0;
    std::memset(a.bbox, 0, sizeof(a.bbox));
    std::memset(b.bbox, 0, sizeof(b.bbox));
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(actual.raster.dthe == expected.raster.dthe);
    CHECK(actual.raster.dimx == expected.raster.dimx);
    CHECK(actual.tex.mxl == expected.tex.mxl);
    CHECK(actual.tex.lcm == expected.tex.lcm);
    CHECK(actual.tex.l == expected.tex.l);
    CHECK(actual.tex.k == expected.tex.k);
    CHECK(std::memcmp(actual.tex.mip_tbp, expected.tex.mip_tbp,
                      sizeof(actual.tex.mip_tbp)) == 0);
    CHECK(std::memcmp(actual.tex.mip_tbw, expected.tex.mip_tbw,
                      sizeof(actual.tex.mip_tbw)) == 0);
}

void CheckSameDraw(const Draw& actual, const Draw& expected) {
    CheckState(actual, expected);
    const auto a = actual.ToDrawRec(), b = expected.ToDrawRec();
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(actual.verts.size() == expected.verts.size());
    if (actual.verts.size() == expected.verts.size() && !actual.verts.empty())
        CHECK(std::memcmp(actual.verts.data(), expected.verts.data(),
                          actual.verts.size() * sizeof(Vertex)) == 0);
}

void CheckVertex(const Vertex& v, float x, float y, uint32_t color, size_t index) {
    CHECK(Bits(v.x) == Bits(x));
    CHECK(Bits(v.y) == Bits(y));
    CHECK(Bits(v.z) == 0x4f7fffffu);  // uint32 full depth, EE drain toward zero
    CHECK(Color(v) == color);
    CHECK(v.draw_kick == (index >= 2 ? 1 : 0));
    CHECK(v.s == 0 && v.t == 0 && v.q == 1 && v.uv_u == 0 && v.uv_v == 0);
    CHECK(v.fog == 0 && v.pad[0] == 0 && v.pad[1] == 0);
}

void TestCountsAndParity() {
    const Draw state = State();
    const Strip::Target target{640, 480, 0, 0};
    Draw offset_state = state;
    offset_state.off_x = 1728; offset_state.off_y = 1808;
    std::array<Strip::Position, 255> positions{};
    std::array<uint32_t, 255> colors{};
    for (size_t i = 0; i < positions.size(); ++i) {
        positions[i] = {float(i), float(i % 2)};
        colors[i] = 0xf3a75900u | uint32_t(i);
    }
    // Count is LBU at NTSC 0x1a9198: test the entire domain, including 0/1/2/255.
    for (size_t count = 0; count <= 255; ++count) {
        Draw draw;
        CHECK(Strip::Build(offset_state, target, positions.data(), colors.data(),
                           count, draw) == Result::Ok);
        CheckState(draw, offset_state);
        CHECK(draw.vertex_count == (count < 3 ? 0 : (count - 2) * 3));
        CHECK(draw.vertex_count == draw.verts.size());
        CHECK(draw.snapshot.verts_incl == 1);
        if (count < 3) {
            CHECK(draw.snapshot.vertex_hash == 0);
            for (float value : draw.bbox) CHECK(value == 0);
        } else {
            CHECK(draw.snapshot.vertex_hash != 0);
            CHECK(draw.bbox[0] == 0 && draw.bbox[1] == 0);
            CHECK(Bits(draw.bbox[2]) == 0x4f7fffffu);
            CHECK(draw.bbox[3] == float(count - 1) && draw.bbox[4] == 1);
            CHECK(Bits(draw.bbox[5]) == 0x4f7fffffu);
        }
        for (size_t newest = 2; newest < count; ++newest) {
            const size_t indices[3] = {
                newest % 2 == 0 ? newest - 2 : newest - 1,
                newest % 2 == 0 ? newest - 1 : newest - 2, newest};
            for (size_t lane = 0; lane < 3; ++lane) {
                const size_t i = indices[lane];
                CheckVertex(draw.verts[(newest - 2) * 3 + lane],
                            positions[i].x, positions[i].y, colors[i], i);
            }
        }
    }
}

void TestFractionsAndTarget() {
    // Source operation order: half dimensions truncated BEFORE centering;
    // multiply by 16, add frame corner, cvt.w.s toward zero, unpack /16, offset.
    // These fixtures pin the reviewed NTSC 0x1a9168..0x1a9224 arithmetic.
    Draw state = State();
    state.off_x = 1728; state.off_y = 1808;
    const Strip::Position positions[] = {{-1.1f, -2.2f}, {0.01f, 0.99f}, {4.5f, 8.125f}};
    const uint32_t colors[] = {0x12345678, 0xff0080c0, 0x01020304};
    Draw even, odd;
    CHECK(Strip::Build(state, {640, 480, 3, -5}, positions, colors, 3, even) == Result::Ok);
    CheckVertex(even.verts[0], 1.875f, -7.25f, colors[0], 0);
    CheckVertex(even.verts[1], 3, -4.0625f, colors[1], 1);
    CheckVertex(even.verts[2], 7.5f, 3.125f, colors[2], 2);
    CHECK(Strip::Build(state, {641, 479, 3, -5}, positions, colors, 3, odd) == Result::Ok);
    for (size_t i = 0; i < 3; ++i)
        CheckVertex(odd.verts[i], even.verts[i].x, even.verts[i].y + 1, colors[i], i);
    CheckState(even, state);
}

void TestSignExtensionAndDepth() {
    const Strip::Position positions[] = {
        {-0.1f, 3.5f}, {1.25f, -0.1f}, {-0.025f, 0.025f},
        {4096.5f, 2}, {0.0625f, -4096.0625f}, {0, 4}};
    const Strip::Position expected[] = {
        {4095.9375f, 4095.9375f}, {1.25f, 4095.9375f}, {0, 0},
        {0.5f, 2.0625f}, {0.0625f, 4095.9375f}, {0, 4}};
    const uint32_t colors[] = {0xffffffff, 0x80000000, 0x00ffffff, 3, 4, 5};
    Draw draw;
    CHECK(Strip::Build(State(), {4096, 4096, 0, 0}, positions, colors, 6, draw) == Result::Ok);
    CHECK(draw.verts.size() == 12);
    const size_t order[] = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5};
    for (size_t i = 0; i < draw.verts.size(); ++i) {
        const size_t input = order[i];
        CheckVertex(draw.verts[i], expected[input].x, expected[input].y, colors[input], input);
    }
}

void TestDegenerates() {
    const uint32_t colors[] = {10, 11, 12, 13, 14, 15};
    const Strip::Target target{4096, 4096, 0, 0};
    const Strip::Position positions[] = {{0, 0}, {0.01f, 0}, {1, 0}, {2, 1}, {3, 0}};
    Draw draw;
    CHECK(Strip::Build(State(), target, positions, colors, 5, draw) == Result::Ok);
    CHECK(draw.verts.size() == 6);
    // The first triangle collapses only AFTER quantization. It still consumes
    // even parity; the first emitted triangle uses odd order (2,1,3).
    const size_t order[] = {2, 1, 3, 2, 3, 4};
    for (size_t i = 0; i < draw.verts.size(); ++i)
        CHECK(Color(draw.verts[i]) == colors[order[i]]);
    const Strip::Position two_degenerate[] = {{0, 0}, {1, 0}, {1, 0}, {2, 1}, {3, 0}, {4, 2}};
    CHECK(Strip::Build(State(), target, two_degenerate, colors, 6, draw) == Result::Ok);
    const size_t second_order[] = {2, 3, 4, 4, 3, 5};
    CHECK(draw.verts.size() == 6);
    for (size_t i = 0; i < draw.verts.size(); ++i)
        CHECK(Color(draw.verts[i]) == colors[second_order[i]]);
    const Strip::Position collinear[] = {{0, 0}, {1, 0}, {2, 0}};
    CHECK(Strip::Build(State(), target, collinear, colors, 3, draw) == Result::Ok);
    CHECK(draw.verts.size() == 3);  // no invented area/culling rejection
    const Strip::Position same[] = {{1, 2}, {1, 2}, {1, 2}};
    CHECK(Strip::Build(State(), target, same, colors, 3, draw) == Result::Ok);
    CHECK(draw.verts.empty() && draw.snapshot.vertex_hash == 0);
    for (float value : draw.bbox) CHECK(value == 0);
}

void TestNumericEndpoints() {
    const float maximum = std::numeric_limits<float>::max();
    const Strip::Position positions[] = {{maximum, 1}, {-maximum, 2},
                                          {3, maximum}, {4, -maximum}};
    const Strip::Position expected[] = {{4095.9375f, 2047.9375f}, {0, 2050},
                                         {3, 4095.9375f}, {4, 0}};
    const uint32_t colors[] = {0x12345678, 0xff00ff00, 0x10203040, 0x50607080};
    Draw draw;
    CHECK(Strip::Build(State(), {4096, 4096, 0, 0}, positions, colors, 4, draw) == Result::Ok);
    CHECK(draw.verts.size() == 6);
    const size_t order[] = {0, 1, 2, 2, 1, 3};
    for (size_t i = 0; i < draw.verts.size(); ++i) {
        const size_t input = order[i];
        CheckVertex(draw.verts[i], expected[input].x, expected[input].y, colors[input], input);
    }
    const Strip::Position small[] = {{1, 1}, {2, 1}, {1, 2}};
    const int32_t big_offset = std::numeric_limits<int32_t>::max();
    CHECK(Strip::Build(State(), {4096, 4096, big_offset, big_offset},
                       small, colors, 3, draw) == Result::Ok);
    for (size_t i = 0; i < 3; ++i)
        CheckVertex(draw.verts[i], small[i].x - 1, small[i].y - 1, colors[i], i);
    Draw state = State();
    state.off_x = 2048; state.off_y = 2048;
    CHECK(Strip::Build(state, {-1, -1, 0, 0}, small, colors, 3, draw) == Result::Ok);
    for (size_t i = 0; i < 3; ++i)
        CheckVertex(draw.verts[i], small[i].x + 1, small[i].y + 1, colors[i], i);
}

void TestOwnershipAndAliases() {
    Draw draw = State();
    std::vector<Strip::Position> positions{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    std::vector<uint32_t> colors{0x12345678, 0xabcdef12, 0xff112233, 0xfedcba98};
    const Strip::Target target{4096, 4096, 0, 0};
    Draw expected;
    CHECK(Strip::Build(draw, target, positions.data(), colors.data(), 4, expected) == Result::Ok);
    CHECK(Strip::Build(draw, target, positions.data(), colors.data(), 4, draw) == Result::Ok);
    CheckSameDraw(draw, expected);  // state == out
    positions.clear(); positions.shrink_to_fit();
    colors.clear(); colors.shrink_to_fit();
    CheckSameDraw(draw, expected);  // independent owned vertices after input release

    // Both inputs reside within the vector that publication replaces.
    Draw backed = State();
    const Strip::Position places[] = {{0, 0}, {1, 0}, {0, 1}};
    const uint32_t rgba[] = {0x80706050, 0x01020304, 0xffeeddcc};
    auto* storage = reinterpret_cast<uint8_t*>(backed.verts.data());
    std::memcpy(storage, places, sizeof(places));
    std::memcpy(storage + sizeof(places), rgba, sizeof(rgba));
    CHECK(Strip::Build(backed, target, places, rgba, 3, expected) == Result::Ok);
    CHECK(Strip::Build(backed, target, reinterpret_cast<const Strip::Position*>(storage),
                      reinterpret_cast<const uint32_t*>(storage + sizeof(places)),
                      3, backed) == Result::Ok);
    CheckSameDraw(backed, expected);

    // Overlapping input views: colours are the x/y words of the position array.
    const Strip::Position overlap[] = {{1, 2}, {3, 4}, {5, 7}};
    uint32_t overlapping_colors[3];
    std::memcpy(overlapping_colors, overlap, sizeof(overlapping_colors));
    CHECK(Strip::Build(State(), target, overlap, overlapping_colors, 3, expected) == Result::Ok);
    CHECK(Strip::Build(State(), target, overlap, reinterpret_cast<const uint32_t*>(overlap),
                      3, draw) == Result::Ok);
    CheckSameDraw(draw, expected);
}

void TestErrorsAndRounding() {
    const Strip::Target target{4096, 4096, 0, 0};
    Strip::Position positions[] = {{0, 0}, {1, 0}, {0, 1}};
    const uint32_t colors[] = {1, 2, 3};
    const Draw seed = State();
    Draw draw = seed;
    CHECK(Strip::Build(seed, target, positions, colors, 256, draw) == Result::InvalidCount);
    CheckSameDraw(draw, seed);
    CHECK(Strip::Build(seed, target, nullptr, colors, 3, draw) == Result::MissingInput);
    CHECK(Strip::Build(seed, target, positions, nullptr, 1, draw) == Result::MissingInput);
    CheckSameDraw(draw, seed);
    for (int invalid = 0; invalid < 6; ++invalid) {
        Draw state = seed;
        if (invalid == 0) state.tex_enabled = true;
        if (invalid == 1) state.prim_type = 3;
        if (invalid == 2) state.ctx = 2;
        if (invalid == 3) state.off_x = -1;
        if (invalid == 4) state.off_y = 0.01f;
        if (invalid == 5) state.off_x = std::numeric_limits<float>::infinity();
        CHECK(Strip::Build(state, target, positions, colors, 3, draw) == Result::UnsupportedState);
        CheckSameDraw(draw, seed);
    }
    const int saved = std::fegetround();
    for (int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD}) {
        std::fesetround(mode);
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()}) {
            positions[2].x = invalid;  // reject after preceding valid reads
            CHECK(Strip::Build(seed, target, positions, colors, 3, draw) ==
                  Result::InvalidCoordinates);
            CHECK(std::fegetround() == mode);
            CheckSameDraw(draw, seed);
        }
    }
    // This x's sum rounds to 44960 with nearest, but below it toward zero:
    // the pixel result must stay 1081.9375 regardless of the caller's mode.
    positions[0] = {1081.9999f, 0};
    positions[1] = {1, 0};
    positions[2] = {0, 1};
    Draw rounding_state = seed;
    rounding_state.off_x = 1728; rounding_state.off_y = 1808;
    const Strip::Target rounding_target{640, 480, 0, 0};
    Draw reference;
    CHECK(Strip::Build(rounding_state, rounding_target, positions, colors,
                       3, reference) == Result::Ok);
    CHECK(reference.verts[0].x == 1081.9375f);
    for (int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD}) {
        std::fesetround(mode);
        CHECK(Strip::Build(rounding_state, rounding_target, positions, colors,
                           3, draw) == Result::Ok);
        CHECK(std::fegetround() == mode);
        // Full uint32 depth rounds down to 2^32 - 256 on the EE drain.
        // Pin every caller mode independently, including the resulting bounds
        // and hash; equality to Build's own reference alone would miss this bug.
        CHECK(draw.verts.size() == 3);
        for (const auto& vertex : draw.verts) {
            CHECK(Bits(vertex.z) == 0x4f7fffffu);
            CHECK(vertex.z == 4294967040.0f);
        }
        CHECK(Bits(draw.bbox[2]) == 0x4f7fffffu);
        CHECK(Bits(draw.bbox[5]) == 0x4f7fffffu);
        auto expected_vertices = draw.verts;
        for (auto& vertex : expected_vertices) vertex.z = 4294967040.0f;
        CHECK(draw.snapshot.vertex_hash ==
              ps2r::gs::GsdlFormat::VertexHash(expected_vertices.data(), expected_vertices.size()));
        CheckSameDraw(draw, reference);
    }
    std::fesetround(saved);
    CHECK(Strip::Build(seed, target, nullptr, nullptr, 0, draw) == Result::Ok);
    CHECK(draw.verts.empty());
}

void TestTintAndPlacement() {
    const Strip::Position positions[] = {{0, 0}, {8, 0}, {0, 4}, {8, 4}};
    const uint32_t colors[] = {0x80c0c0c0, 0xffffffff, 0x40203040, 0x00102030};
    const Strip::Target target{640, 480, 7, 11};
    Draw state = State();
    state.off_x = 1728; state.off_y = 1808;
    // Reflect/scale, translate, and deliberately make result.w nonunit.
    // The 2D placed contract takes x/y directly; it never divides by w.
    const Strip::Matrix matrix{{{-2, 0, 0, 1}, {0, 3, 0, 2},
                                 {0, 0, 1, 3}, {5, -7, 0, 4}}};
    const Strip::Effects effects{&matrix, true, 0x40606060};
    const uint32_t expected[] = {0x40606060, 0x7f7f7f7f, 0x20101820, 0x00081018};
    const Strip::Position moved[] = {{12, 4}, {-4, 4}, {12, 16}, {-4, 16}};
    const size_t order[] = {0, 1, 2, 2, 1, 3};
    const int saved = std::fegetround();
    for (int mode : {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD}) {
        std::fesetround(mode);
        Draw draw;
        CHECK(Strip::Build(state, target, positions, colors, 4, effects, draw) == Result::Ok);
        CHECK(std::fegetround() == mode);
        CHECK(draw.verts.size() == 6);
        CheckState(draw, state);
        for (size_t i = 0; i < draw.verts.size(); ++i) {
            const auto vertex = order[i];
            CheckVertex(draw.verts[i], moved[vertex].x, moved[vertex].y,
                        expected[vertex], vertex);
        }
    }
    std::fesetround(saved);

    // Bright tints wrap after source truncation. Saturating to 255 is wrong.
    const uint32_t bright[] = {0xffffffff, 0xffffffff, 0xffffffff};
    Draw draw;
    CHECK(Strip::Build(state, target, positions, bright, 3,
                       {nullptr, true, 0xffffffff}, draw) == Result::Ok);
    for (const auto& vertex : draw.verts) CHECK(Color(vertex) == 0xfc525252u);
    CHECK(Strip::Build(state, target, positions, colors, 4,
                       {nullptr, true, 0}, draw) == Result::Ok);
    for (const auto& vertex : draw.verts) CHECK(Color(vertex) == 0);

    // Transform-induced collapsed vertices consume strip parity as usual.
    const Strip::Matrix collapse{{{0, 0, 0, 0}, {0, 0, 0, 0},
                                   {0, 0, 1, 0}, {5, 6, 0, 1}}};
    CHECK(Strip::Build(state, target, positions, colors, 4,
                       {&collapse, false, 0}, draw) == Result::Ok);
    CHECK(draw.verts.empty() && draw.snapshot.vertex_hash == 0);
}

void TestEffectsOwnershipAndFailure() {
    const Strip::Target target{4096, 4096, 0, 0};
    const Strip::Position positions[] = {{1, 0}, {0, 1}, {1, 1}};
    const uint32_t colors[] = {0x80808080, 0x80c0c0c0, 0xffffffff};
    Strip::Matrix matrix{{{1, 0, 0, 0}, {0, 1, 0, 0},
                           {0, 0, 1, 0}, {5, 6, 0, 1}}};
    Draw seed = State(), expected;
    CHECK(Strip::Build(seed, target, positions, colors, 3,
                       {&matrix, true, 0x40606060}, expected) == Result::Ok);
    // Matrix and both arrays live in storage destroyed by output publication.
    Draw backed = seed;
    auto* storage = reinterpret_cast<uint8_t*>(backed.verts.data());
    std::memcpy(storage, &matrix, sizeof(matrix));
    std::memcpy(storage + sizeof(matrix), positions, sizeof(positions));
    std::memcpy(storage + sizeof(matrix) + sizeof(positions), colors, sizeof(colors));
    CHECK(Strip::Build(backed, target,
                       reinterpret_cast<const Strip::Position*>(storage + sizeof(matrix)),
                       reinterpret_cast<const uint32_t*>(storage + sizeof(matrix) + sizeof(positions)),
                       3, {reinterpret_cast<const Strip::Matrix*>(storage), true, 0x40606060},
                       backed) == Result::Ok);
    CheckSameDraw(backed, expected);
    matrix = {};
    CheckSameDraw(backed, expected);

    for (size_t column = 0; column != 4; ++column)
        for (size_t lane = 0; lane != 4; ++lane) {
            matrix = {};
            matrix[column][lane] = std::numeric_limits<float>::quiet_NaN();
            Draw actual = seed;
            CHECK(Strip::Build(seed, target, positions, colors, 3,
                               {&matrix, false, 0}, actual) == Result::InvalidTransform);
            CheckSameDraw(actual, seed);
        }
}

}  // namespace

int main() {
    TestCountsAndParity();
    TestFractionsAndTarget();
    TestSignExtensionAndDepth();
    TestDegenerates();
    TestNumericEndpoints();
    TestOwnershipAndAliases();
    TestErrorsAndRounding();
    TestTintAndPlacement();
    TestEffectsOwnershipAndFailure();
    std::printf("StripDraw: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
