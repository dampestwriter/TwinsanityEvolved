#include "games/slus_20909_crash_twinsanity/tests/title/effects/wave_oracle.h"

#include <cfenv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
using Wave = title::WaveGeometry;
void Compare(const std::vector<ps2r::gs::DrawIr>& a, const std::vector<ps2r::gs::DrawIr>& b) {
    CHECK(a.size() == b.size());
    if (a.size() != b.size()) return;
    for (size_t strip = 0; strip < a.size(); ++strip) {
        CHECK(a[strip].verts.size() == b[strip].verts.size());
        if (a[strip].verts.size() != b[strip].verts.size()) continue;
        for (size_t i = 0; i < a[strip].verts.size(); ++i) {
            const auto& left = a[strip].verts[i];
            const auto& right = b[strip].verts[i];
            if (left.x != right.x || left.y != right.y || left.z != right.z) {
                std::fprintf(stderr, "strip%zu vertex%zu: NEW %.8g %.8g %.8g, wave %.8g %.8g %.8g\n",
                             strip, i, left.x, left.y, left.z, right.x, right.y, right.z);
                CHECK(false);
                return;
            }
            CHECK(left.uv_u == right.uv_u && left.uv_v == right.uv_v);
            CHECK(left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a);
            CHECK(left.s == right.s && left.t == right.t && left.q == right.q);
        }
    }
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::puts("SKIP: statically extracted NTSC type-1C image required"); return 77; }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    std::vector<uint8_t> program{std::istreambuf_iterator<char>(input), {}};
    WaveOracle oracle(program);
    program[0] ^= 1;
    bool rejected = false;
    try { WaveOracle invalid(program); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    Wave::Grid grid;
    CHECK(Wave::MakeGrid(512, 448, grid) == Wave::Result::Ok);
    CHECK(grid.strips.size() == 64);
    CHECK(grid.strips[0].vertices.size() == 32 && grid.strips[32].vertices.size() == 34);
    CHECK(grid.strips[0].vertices.back().position[1] == grid.strips[32].vertices[0].position[1]);
    Wave::Waves waves;
    for (size_t row = 0; row < 16; ++row)
        waves.rows[row] = {float(row) / 16 - .5f, float(15 - row) / 15 - .5f, float(row)};
    for (float amplitude : {0.f, .03125f, .5f, 4.f, -3.f, 128.f}) {
        waves.amplitude = amplitude;
        std::vector<ps2r::gs::DrawIr> replacement;
        std::fesetround(FE_UPWARD);
        CHECK(Wave::Build(grid, waves, 5, replacement) == Wave::Result::Ok);
        CHECK(std::fegetround() == FE_UPWARD);
        Compare(oracle.Run(grid, waves), replacement);
    }
    std::fesetround(FE_TONEAREST);
    // Original source dimensions change offsets and truncate the row stride.
    for (uint32_t height : {32u, 447u, 512u}) {
        CHECK(Wave::MakeGrid(640, height, grid) == Wave::Result::Ok);
        waves.amplitude = 3.5f;
        std::vector<ps2r::gs::DrawIr> replacement;
        CHECK(Wave::Build(grid, waves, 6, replacement) == Wave::Result::Ok);
        Compare(oracle.Run(grid, waves), replacement);
    }
    std::vector<ps2r::gs::DrawIr> sentinel(1);
    waves.amplitude = std::numeric_limits<float>::quiet_NaN();
    CHECK(Wave::Build(grid, waves, 0, sentinel) == Wave::Result::InvalidWaves);
    CHECK(sentinel.size() == 1);
    waves.amplitude = 0;
    grid.strips[0].vertices[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK(Wave::Build(grid, waves, 0, sentinel) == Wave::Result::InvalidGeometry);
    CHECK(sentinel.size() == 1);
    std::printf("WaveGeometry: %d checks, %d failures (static NTSC program/current NEW interpreter)\n",
                checks, failures);
    return failures ? 1 : 0;
}
