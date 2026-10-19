#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_oracle.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using Geometry = title::DistortionGeometry;
unsigned checks = 0, cases = 0, triangles = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
float Pack(float shape, uint8_t colour) {
    uint32_t bits; std::memcpy(&bits, &shape, 4);
    bits = (bits & ~255u) | colour;
    std::memcpy(&shape, &bits, 4);
    return shape;
}
Geometry::Look MakeLook() {
    Geometry::Look look;
    look.gravity = -2;
    look.corners = {{{.25f,0}, {.125f,.21650635f}, {-.125f,.21650635f},
                     {-.25f,0}, {-.125f,-.21650635f}, {.125f,-.21650635f}}};
    for (unsigned i = 0; i < 64; ++i) {
        const float turn = float(i) / 63;
        auto& shape = look.shapes[i];
        shape = {-400 - turn*50, -300 + turn*150, 800 + turn*100, turn*200, turn*300, 600 - turn*100};
        for (unsigned lane = 0; lane < 4; ++lane)
            shape[lane] = Pack(shape[lane], uint8_t(i*7 + lane*30));
    }
    return look;
}
Geometry::View MakeView() {
    Geometry::View view;
    view.time = 2;
    view.distortion = {3, 7};
    view.screen = {{{16,0,0,0}, {0,16,0,0}, {0,0,16,.01f}, {32768,32768,40000,1}}};
    return view;
}
ps2r::gs::DrawIr Material() {
    ps2r::gs::DrawIr draw;
    draw.prim_type = 4;
    draw.tex_enabled = draw.gouraud = true;
    draw.off_x = 1792; draw.off_y = 1824;
    return draw;
}
std::vector<Geometry::Block> MakeBlocks() {
    std::vector<Geometry::Block> blocks(2);
    for (unsigned b = 0; b < blocks.size(); ++b) for (unsigned i = 0; i < 12; ++i)
        blocks[b][i] = {{float(int(i)-6)*2, float(i%3)*2, float(b)*20},
                        .1f*float(i), {1.f+float(b),-.5f,float(i%4)}, 3.f+float(i)};
    return blocks;
}
void Compare(const DistortionOracle& oracle, const std::vector<Geometry::Block>& blocks,
             const Geometry::Look& look, const Geometry::View& view, const ps2r::gs::DrawIr& state) {
    std::vector<ps2r::gs::DrawIr> actual;
    std::fesetround(FE_UPWARD);
    CHECK(Geometry::Build(blocks, look, view, state, actual) == Geometry::Result::Ok);
    CHECK(std::fegetround() == FE_UPWARD);
    const auto expected = oracle.Run(blocks, look, view, state);
    CHECK(std::fegetround() == FE_UPWARD);
    std::fesetround(FE_TONEAREST);
    ++cases;
    CHECK(expected.size() == actual.size());
    for (size_t group = 0; group < expected.size(); ++group) {
        CHECK(expected[group].verts.size() == actual[group].verts.size());
        triangles += unsigned(actual[group].verts.size() / 3);
        for (size_t i = 0; i < expected[group].verts.size(); ++i) {
            const auto& a = expected[group].verts[i];
            const auto& b = actual[group].verts[i];
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.s != b.s || a.t != b.t || a.q != b.q ||
                a.draw_kick != b.draw_kick) {
                std::fprintf(stderr, "case%u group%zu v%zu NEW %.9g %.9g %.9g %.9g %.9g %.9g %u"
                    " owned %.9g %.9g %.9g %.9g %.9g %.9g %u\n", cases, group, i,
                    a.x,a.y,a.z,a.s,a.t,a.q,a.draw_kick,b.x,b.y,b.z,b.s,b.t,b.q,b.draw_kick);
                CHECK(false);
            }
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a)
                std::fprintf(stderr, "case%u group%zu v%zu RGBA NEW %u %u %u %u owned %u %u %u %u\n",
                    cases, group, i, a.r,a.g,a.b,a.a,b.r,b.g,b.b,b.a);
            CHECK(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a);
            CHECK(a.uv_u == b.uv_u && a.uv_v == b.uv_v && a.fog == b.fog);
        }
    }
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::puts("SKIP: verified static NTSC type18 image required"); return 77; }
    try {
        std::ifstream file(argv[1], std::ios::binary);
        CHECK(bool(file));
        std::vector<uint8_t> program{std::istreambuf_iterator<char>(file), {}};
        DistortionOracle oracle(program);
        program[3] ^= 1;
        bool refused = false;
        try { DistortionOracle changed(program); } catch (const std::invalid_argument&) { refused = true; }
        CHECK(refused);
        auto look = MakeLook();
        auto view = MakeView();
        auto blocks = MakeBlocks();
        const auto state = Material();
        Compare(oracle, blocks, look, view, state);
        for (float time : {-1.f, 0.f, 1.f, 5.f, 20.f, 100.f}) {
            view.time = time;
            Compare(oracle, blocks, look, view, state);
        }
        view = MakeView();
        // OLD VU1_HLE_R3: the three distinct sign boundaries are not a single
        // approximate lifetime/near-plane condition.
        for (float phase : {-0.f, 0.f, .00001f, 1.f, 1.125f, 62.f, 62.89999f, 62.9f, 62.90001f, 63.f}) {
            for (auto& block : blocks) for (auto& particle : block) {
                particle.spawn_time = 0;
                particle.life_scale = 1;
            }
            view.time = phase;
            Compare(oracle, blocks, look, view, state);
        }
        blocks = MakeBlocks();
        view = MakeView();
        for (float edge : {std::nextafter(62.9f, 0.f), std::nextafter(62.9f, 100.f),
                           std::numeric_limits<float>::denorm_min(),
                           -std::numeric_limits<float>::denorm_min()}) {
            for (auto& block : blocks) for (auto& particle : block) {
                particle.spawn_time = 0;
                particle.life_scale = 1;
            }
            view.time = edge;
            Compare(oracle, blocks, look, view, state);
        }
        blocks = MakeBlocks();
        view = MakeView();
        for (float depth : {-.1f, -0.f, 0.f, .49999f, .5f, .50001f, 1.f, 39.99f, 40.f, 40.01f, 100.f}) {
            view.screen[2][3] = 0;
            view.screen[3][3] = depth;
            Compare(oracle, blocks, look, view, state);
        }
        view = MakeView();
        for (float distortion : {-100.f, -1.f, -0.f, 0.f, 1.f, 100.f}) {
            view.distortion = {distortion, distortion * -3};
            Compare(oracle, blocks, look, view, state);
        }
        // Screen clamp, non-square copy, and source colour bits embedded in
        // the shape all affect actual emitted vertices/STQ.
        for (unsigned width : {320u, 512u, 640u}) for (unsigned height : {240u, 448u, 512u}) {
            view.width = width; view.height = height;
            view.screen[3][0] = width == 320 ? -500.f : width == 640 ? 70000.f : 32768.f;
            Compare(oracle, blocks, look, view, state);
        }
        view = MakeView();
        for (float gravity : {-100.f, -0.f, 0.f, 10.f}) {
            look.gravity = gravity;
            view.screen[0][1] = 3;
            view.screen[1][0] = -2;
            view.screen[3][3] = 3;
            Compare(oracle, blocks, look, view, state);
        }
        // The two ordered radial strips carry their own alpha gradient.
        look = MakeLook(); view = MakeView(); blocks = MakeBlocks();
        std::vector<ps2r::gs::DrawIr> gradient;
        CHECK(Geometry::Build(blocks, look, view, state, gradient) == Geometry::Result::Ok);
        CHECK(gradient.size() == 8 && gradient.front().verts.size() == 54);
        CHECK(!gradient[0].verts[0].a && !gradient[0].verts[1].a && gradient[0].verts[2].a == 132);
        CHECK(triangles > 4000);
        view = MakeView();
        auto sentinel = std::vector<ps2r::gs::DrawIr>(1);
        look.shapes[0][0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(Geometry::Build(blocks, look, view, state, sentinel) == Geometry::Result::InvalidValues);
        CHECK(sentinel.size() == 1);
        look = MakeLook(); blocks[0][0].life_scale = -1;
        CHECK(Geometry::Build(blocks, look, view, state, sentinel) == Geometry::Result::InvalidValues);
        blocks = MakeBlocks(); view.width = 0;
        CHECK(Geometry::Build(blocks, look, view, state, sentinel) == Geometry::Result::InvalidDimensions);
        view = MakeView(); auto wrong = state; wrong.use_fst = true;
        CHECK(Geometry::Build(blocks, look, view, wrong, sentinel) == Geometry::Result::UnsupportedState);
        CHECK(sentinel.size() == 1);
        std::printf("DistortionGeometry: %u checks/%u cases/%u triangles, original 212-instruction NEW oracle\n",
                    checks, cases, triangles);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
