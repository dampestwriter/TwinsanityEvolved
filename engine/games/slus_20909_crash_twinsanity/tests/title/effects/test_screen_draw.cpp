#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_draw.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/wave_geometry.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/screen_oracle.h"

#include <cstdio>
#include <tuple>

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
using Draw = ps2r::gs::DrawIr;
void State(const Draw& a, const Draw& b) {
    CHECK(a.prim_type == b.prim_type && a.use_fst == b.use_fst);
    CHECK(a.tex_enabled == b.tex_enabled && a.alpha_blend == b.alpha_blend);
    CHECK(a.raster.frame.fbw == b.raster.frame.fbw && a.raster.frame.psm == b.raster.frame.psm);
    CHECK(a.raster.frame.fbmsk == b.raster.frame.fbmsk);
    CHECK(a.raster.zbuf.psm == b.raster.zbuf.psm && a.raster.zbuf.zmsk == b.raster.zbuf.zmsk);
    CHECK(a.raster.test.zte == b.raster.test.zte && a.raster.test.ztst == b.raster.test.ztst);
    CHECK(a.raster.test.ate == b.raster.test.ate && a.raster.test.date == b.raster.test.date);
    CHECK(a.raster.colclamp == b.raster.colclamp && a.raster.fba == b.raster.fba);
    CHECK(a.raster.dthe == b.raster.dthe && a.raster.pabe == b.raster.pabe);
    CHECK(a.raster.scissor.x0 == b.raster.scissor.x0 && a.raster.scissor.x1 == b.raster.scissor.x1);
    CHECK(a.raster.scissor.y0 == b.raster.scissor.y0 && a.raster.scissor.y1 == b.raster.scissor.y1);
    CHECK(a.tex.psm == b.tex.psm && a.tex.tbw == b.tex.tbw);
    CHECK(a.tex.tw == b.tex.tw && a.tex.th == b.tex.th);
    CHECK(a.tex.tcc == b.tex.tcc && a.tex.tfx == b.tex.tfx);
    CHECK(a.tex.mmag == b.tex.mmag && a.tex.mmin == b.tex.mmin);
    CHECK(a.tex.wms == b.tex.wms && a.tex.wmt == b.tex.wmt);
    CHECK(a.tex.texa_ta0 == b.tex.texa_ta0 && a.tex.texa_ta1 == b.tex.texa_ta1);
    CHECK(a.tex.cld == b.tex.cld);
    if (a.alpha_blend) {
        CHECK(a.raster.alpha.a == b.raster.alpha.a && a.raster.alpha.b == b.raster.alpha.b);
        CHECK(a.raster.alpha.c == b.raster.alpha.c && a.raster.alpha.d == b.raster.alpha.d);
        CHECK(a.raster.alpha.fix == b.raster.alpha.fix);
    }
}
auto Corner(const ps2r::gs::VertexRec& v) {
    return std::make_tuple(v.x, v.y, v.z, v.uv_u, v.uv_v, v.r, v.g, v.b, v.a);
}
void Geometry(const Draw& a, const Draw& b) {
    CHECK(a.verts.size() == b.verts.size());
    if (a.verts.size() != b.verts.size()) return;
    // Preserve triangle order as well as corners: raster interpolation can
    // round differently when an otherwise identical triangle is rotated.
    for (size_t i = 0; i < a.verts.size(); ++i) CHECK(Corner(a.verts[i]) == Corner(b.verts[i]));
}
}
int main() {
    using D = title::ScreenDraw;
    for (uint32_t height : {32u, 256u, 447u, 448u, 512u, 1024u}) {
        const auto blur = ScreenOracle::Blur(height);
        std::vector<Draw> replacement;
        CHECK(D::Blur(height, 7, replacement) == D::Result::Ok);
        CHECK(blur.size() == 3 && blur.size() == replacement.size());
        for (size_t i = 0; i < blur.size() && i < replacement.size(); ++i) {
            State(blur[i], replacement[i]);
            Geometry(blur[i], replacement[i]);
        }
        for (bool after_blur : {false, true}) {
            const auto original = ScreenOracle::Filter(height, after_blur);
            Draw filter;
            CHECK(D::Filter(height, 7, after_blur, filter) == D::Result::Ok);
            State(original, filter);
            Geometry(original, filter);
        }
        for (uint32_t width : {512u, 640u}) {
            const auto wave = ScreenOracle::Wave(width, height);
            CHECK(wave.size() == 2);
            if (wave.size() != 2) continue;
            Draw copy;
            CHECK(D::WaveCopy(height, 7, copy) == D::Result::Ok);
            State(wave[0], copy);
            Geometry(wave[0], copy);
            title::WaveGeometry::Grid grid;
            CHECK(title::WaveGeometry::MakeGrid(width, height, grid) == title::WaveGeometry::Result::Ok);
            CHECK(title::WaveGeometry::Build(grid, {}, 7, replacement) == title::WaveGeometry::Result::Ok);
            CHECK(!replacement.empty());
            if (!replacement.empty()) State(wave[1], replacement.front());
        }
    }
    Draw sentinel;
    sentinel.src_frame = 73;
    CHECK(D::WaveCopy(0, 0, sentinel) == D::Result::InvalidHeight && sentinel.src_frame == 73);
    CHECK(D::Filter(1025, 0, false, sentinel) == D::Result::InvalidHeight && sentinel.src_frame == 73);
    std::printf("ScreenDraw: %d checks, %d failures; NEW body/callee calls %llu\n",
                checks, failures, (unsigned long long)ScreenOracle::Calls());
    return failures ? 1 : 0;
}
