#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_material_oracle.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"
#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"

#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace {
using M = title::DistortionMaterial;
using Draw = ps2r::gs::DrawIr;
namespace ram = strip_oracle::memory;
unsigned checks = 0, cases = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion material case ") + std::to_string(cases) + " line " + \
    std::to_string(__LINE__) + ": " + #value); } while (0)
void Same(Draw expected, Draw actual, bool copy) {
    // Routing annotations are resolved by explicit image owners. Compare the
    // complete active state and geometry independently of those addresses.
    const auto normalize = [copy](Draw& draw) {
        draw.raster.frame.fbp = draw.raster.zbuf.zbp = draw.tex.tbp = 0;
        if (!draw.alpha_blend) draw.raster.alpha = {};
        draw.seq = draw.vertex_count = 0; draw.snapshot = {};
        for (float& value : draw.bbox) value = 0;
        if (!copy) draw.verts.clear();
    };
    normalize(expected); normalize(actual);
    const auto a = expected.ToDrawRec(), b = actual.ToDrawRec();
    if (std::memcmp(&a, &b, sizeof(a))) {
        const auto* left = reinterpret_cast<const uint8_t*>(&a), *right = reinterpret_cast<const uint8_t*>(&b);
        for (size_t i = 0; i < sizeof(a); ++i)
            if (left[i] != right[i]) std::fprintf(stderr, "state byte%zu NEW %02x replacement %02x\n",i,left[i],right[i]);
    }
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(expected.raster.dthe == actual.raster.dthe && expected.raster.dimx == actual.raster.dimx);
    CHECK(std::tie(expected.tex.mmag,expected.tex.mmin,expected.tex.mxl,expected.tex.l,expected.tex.k) ==
          std::tie(actual.tex.mmag,actual.tex.mmin,actual.tex.mxl,actual.tex.l,actual.tex.k));
    CHECK(expected.verts.size() == actual.verts.size());
    // Preserve the source's triangle order, including provoking vertex and
    // interpolation rounding, rather than comparing an unordered corner bag.
    const auto key = [](const ps2r::gs::VertexRec& v) {
        return std::make_tuple(v.x,v.y,v.z,v.s,v.t,v.q,v.r,v.g,v.b,v.a,v.uv_u,v.uv_v);
    };
    for (size_t i = 0; i < expected.verts.size(); ++i) CHECK(key(expected.verts[i]) == key(actual.verts[i]));
}
}
int main() {
    try {
        constexpr uint64_t Base = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 | uint64_t{1} << 59;
        DistortionMaterialOracle oracle;
        for (unsigned variant = 0; variant < 64; ++variant) {
            ++cases;
            const uint64_t attributes = Base | uint64_t(variant % 16) << 1 |
                uint64_t(variant % 2) << 55 | uint64_t(variant % 3 != 0) << 56 |
                (variant & 4 ? uint64_t{1} << 38 | uint64_t{1} << 41 | uint64_t{2} << 43 : 0);
            const M::Settings settings{attributes, int16_t(variant % 2 ? -32768 : 69),
                int16_t(variant % 3 ? 1 : -1)};
            oracle.Reset(settings, variant % 3 ? 448 : 480, variant);
            M::Settings captured;
            CHECK(M::CaptureSettings(oracle.Reader(), oracle.kShader, captured) == M::Result::Ok);
            CHECK(captured.attributes == settings.attributes && captured.lod_k == settings.lod_k &&
                  captured.lod_l == settings.lod_l);
            const auto original = oracle.Original(true);
            Draw material, copy;
            CHECK(M::Build(captured, oracle.frame, oracle.target, 0, material) == M::Result::Ok);
            CHECK(title::ScreenDraw::DistortionCopy(oracle.frame.display_height, 0,
                oracle.frame.environment, copy) == title::ScreenDraw::Result::Ok);
            Same(original[0], copy, true);
            Same(original[1], material, false);
            CHECK(!material.raster.dthe); // The actual preceding copy disabled it.
        }
        oracle.Reset({Base, 0, 0}, 448, 1);
        M::Settings captured;
        CHECK(M::CaptureSettings(oracle.Reader(), oracle.kShader, captured) == M::Result::Ok);
        const auto original = oracle.Original(true);
        Draw retained;
        CHECK(M::Build(captured, oracle.frame, oracle.target, 0, retained) == M::Result::Ok);
        Same(original[1], retained, false);
        CHECK(!retained.tex.mmag && !retained.tex.k && !retained.tex.l);
        CHECK((ram::read<uint64_t>(oracle.kShader) & (uint64_t{1} << 55)) &&
              ram::read<int16_t>(oracle.kShader + 0x18) == -200);
        captured = {123, 456, 789};
        for (auto at : {oracle.kShader, oracle.kShader + 0x6f, M::kVtable + 0x2c}) {
            CHECK(M::CaptureSettings(oracle.Reader(at), oracle.kShader, captured) == M::Result::InvalidMemory);
            CHECK(captured.attributes == 123 && captured.lod_k == 456 && captured.lod_l == 789);
        }
        CHECK(M::CaptureSettings(oracle.Reader(), 0x1ffffff0, captured) == M::Result::InvalidMemory);
        ram::write<uint32_t>(M::kVtable + 0x2c, 0x1d18c8);
        CHECK(M::CaptureSettings(oracle.Reader(), oracle.kShader, captured) == M::Result::UnsupportedSettings);
        Draw sentinel; sentinel.src_frame = 123;
        CHECK(M::Build({uint64_t{1} << 30, 0, 0}, oracle.frame, oracle.target, 0, sentinel) ==
              M::Result::UnsupportedSettings);
        auto target = oracle.target; target[1] = 2047;
        CHECK(M::Build({Base,0,0}, oracle.frame, target, 0, sentinel) == M::Result::UnsupportedTarget);
        CHECK(sentinel.src_frame == 123);
        CHECK(title::ScreenDraw::DistortionCopy(0, 0, oracle.frame.environment, sentinel) ==
              title::ScreenDraw::Result::InvalidHeight);
        CHECK(sentinel.src_frame == 123);
        std::printf("DistortionMaterial: %u checks/%u cases/%llu complete generated calls; settings clock pinned\n",
            checks, cases, (unsigned long long)DistortionMaterialOracle::Calls());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
