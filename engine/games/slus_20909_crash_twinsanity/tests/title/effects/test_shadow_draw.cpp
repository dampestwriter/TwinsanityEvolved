#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_draw.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_oracle.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <tuple>

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)

// Logical sprite corners, independent of the backend's equivalent triangle order.
auto Corner(const ps2r::gs::VertexRec& v) {
    return std::make_tuple(v.x, v.y, v.z, v.uv_u, v.uv_v, v.r, v.g, v.b, v.a);
}
void Compare(const ps2r::gs::DrawIr& original, const ps2r::gs::DrawIr& replacement) {
    CHECK(original.prim_type == replacement.prim_type);
    CHECK(original.alpha_blend == replacement.alpha_blend);
    CHECK(original.tex_enabled == replacement.tex_enabled);
    CHECK(original.raster.frame.fbw == replacement.raster.frame.fbw);
    CHECK(original.raster.frame.psm == replacement.raster.frame.psm);
    CHECK(original.raster.frame.fbmsk == replacement.raster.frame.fbmsk);
    CHECK(original.raster.zbuf.zmsk == replacement.raster.zbuf.zmsk);
    CHECK(original.raster.test.zte == replacement.raster.test.zte);
    CHECK(original.raster.test.ztst == replacement.raster.test.ztst);
    CHECK(original.raster.test.date == replacement.raster.test.date);
    CHECK(original.raster.dthe == replacement.raster.dthe);
    CHECK(original.raster.fba == replacement.raster.fba);
    if (original.alpha_blend) {
        CHECK(original.raster.alpha.a == replacement.raster.alpha.a);
        CHECK(original.raster.alpha.b == replacement.raster.alpha.b);
        CHECK(original.raster.alpha.c == replacement.raster.alpha.c);
        CHECK(original.raster.alpha.d == replacement.raster.alpha.d);
        CHECK(original.raster.alpha.fix == replacement.raster.alpha.fix);
    }
    if (original.tex_enabled) {
        CHECK(original.tex.psm == replacement.tex.psm);
        CHECK(original.tex.tw == replacement.tex.tw && original.tex.th == replacement.tex.th);
        CHECK(original.tex.tcc == replacement.tex.tcc && original.tex.tfx == replacement.tex.tfx);
        CHECK(original.tex.mmag == replacement.tex.mmag && original.tex.mmin == replacement.tex.mmin);
        CHECK(original.tex.wms == replacement.tex.wms && original.tex.wmt == replacement.tex.wmt);
        CHECK(original.tex.texa_ta0 == replacement.tex.texa_ta0);
        CHECK(original.tex.texa_ta1 == replacement.tex.texa_ta1);
        auto left = original.verts, right = replacement.verts;
        auto less = [](const auto& a, const auto& b) { return Corner(a) < Corner(b); };
        std::sort(left.begin(), left.end(), less);
        std::sort(right.begin(), right.end(), less);
        CHECK(left.size() == right.size());
        if (left.size() == right.size())
            for (size_t i = 0; i < left.size(); ++i) CHECK(Corner(left[i]) == Corner(right[i]));
    }
    CHECK(original.verts.size() == replacement.verts.size());
}
}
int main() {
    using D = title::ShadowDraw;
    for (const uint32_t width : {128u, 256u, 512u, 640u, 1024u}) {
        const auto original = ShadowOracle::Passes(width, 448);
        std::vector<D::Command> output;
        CHECK(D::Build({7, width, 448}, {}, output) == D::Result::Ok);
        CHECK(output.size() == original.size());
        for (size_t i = 0; i < original.size(); ++i) Compare(original[i], output[i].draw);
    }
    std::vector<D::Command> output{{D::Step::Composite, {}}};
    CHECK(D::Build({0, 0, 448}, {}, output) == D::Result::InvalidFrame);
    CHECK(output.size() == 1);
    ps2r::gs::DrawIr volume;
    volume.verts.resize(3);
    for (auto& v : volume.verts) { v.r = v.g = v.b = 8; v.z = 65536; }
    CHECK(D::Build({0, 512, 448}, {volume}, output) == D::Result::Ok);
    CHECK(output.size() == 7 && output[4].step == D::Step::Volume);
    CHECK(output[4].draw.raster.test.date && !output[4].draw.raster.test.datm);
    CHECK(!output[4].draw.raster.colclamp);
    volume.verts[1].b = 248;
    CHECK(D::Build({0, 512, 448}, {volume}, output) == D::Result::InvalidVolume);
    volume.verts[1].b = 8;
    volume.verts[1].x = std::numeric_limits<float>::infinity();
    CHECK(D::Build({0, 512, 448}, {volume}, output) == D::Result::InvalidVolume);
    CHECK(D::CounterByte(0) == 0x78 && D::CounterByte(1) == 0x80);
    CHECK(D::CounterByte(32) == D::CounterByte(0));
    CHECK(D::QuantizedByte(0) == 0 && D::QuantizedByte(1) == 0x84);
    CHECK(D::Darken(0xc8, 0x84) == 0xa7 && D::Darken(16, 0x84) == 0);
    CHECK(ShadowOracle::Calls() >= 10);
    std::printf("ShadowDraw: %d checks, %d failures; NEW body calls %llu\n",
                checks, failures, (unsigned long long)ShadowOracle::Calls());
    return failures ? 1 : 0;
}
