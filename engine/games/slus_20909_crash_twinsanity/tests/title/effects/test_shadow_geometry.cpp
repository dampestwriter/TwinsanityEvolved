#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_geometry_oracle.h"

#include <cfenv>
#include <cstdio>
#include <limits>
#include <tuple>

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
using G = title::ShadowGeometry;
auto Corner(const ps2r::gs::VertexRec& v) { return std::make_tuple(v.x, v.y, v.z, v.r, v.g, v.b, v.a); }
void Compare(const G::Mesh& mesh, const std::vector<G::View>& views) {
    const auto original = ShadowGeometryOracle::Run(mesh, views);
    std::vector<ps2r::gs::DrawIr> replacement;
    std::fesetround(FE_UPWARD);
    CHECK(G::Build(mesh, views, replacement) == G::Result::Ok);
    CHECK(std::fegetround() == FE_UPWARD);
    std::fesetround(FE_TONEAREST);
    CHECK(original.size() == replacement.size());
    if (original.size() != replacement.size()) {
        std::fprintf(stderr, "groups NEW %zu replacement %zu\n", original.size(), replacement.size());
        return;
    }
    for (size_t group = 0; group < original.size(); ++group) {
        const auto& a = original[group]; const auto& b = replacement[group];
        CHECK(a.prim_type == b.prim_type && a.verts.size() == b.verts.size());
        if (a.verts.size() != b.verts.size()) continue;
        for (size_t i = 0; i < a.verts.size(); ++i) {
            CHECK(Corner(a.verts[i]) == Corner(b.verts[i]));
            if (Corner(a.verts[i]) != Corner(b.verts[i])) {
                std::fprintf(stderr, "group %zu vertex %zu NEW %.9g %.9g %.9g %u, own %.9g %.9g %.9g %u\n",
                    group, i, a.verts[i].x, a.verts[i].y, a.verts[i].z, a.verts[i].r,
                    b.verts[i].x, b.verts[i].y, b.verts[i].z, b.verts[i].r);
                break;
            }
        }
    }
}
G::View View() {
    G::View view;
    view.screen = {{{1280, 0, 0, 0}, {0, 960, 0, 0}, {0, 0, -65536, .125f},
                    {32768, 32768, -2147483520.f, 1}}};
    view.clip = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    view.guard = {1, 1, 1, 1};
    view.offset = {1920, 1936};
    return view;
}
}
int main() {
    G::Mesh mesh{{G::Strip{{{{-.5f, -.5f, .1f}, true}, {{.5f, -.5f, .2f}, true},
                             {{-.5f, .5f, .3f}}, {{.5f, .5f, .4f}},
                             {{-.7f, .6f, .5f}}, {{.7f, .6f, .6f}}}}}};
    Compare(mesh, {View()});
    auto translated = View();
    translated.screen[3][0] += 31.0625f;
    Compare(mesh, {View(), translated});
    // All six planes; the full fan path uses each source orientation variant.
    for (unsigned axis = 0; axis < 3; ++axis)
        for (float outside : {-1.75f, 1.75f}) {
            auto clipped = mesh;
            clipped.strips[0].vertices[2].position[axis] = outside;
            Compare(clipped, {View()});
            for (auto& vertex : clipped.strips[0].vertices) vertex.reverse_fan = true;
            Compare(clipped, {View()});
        }
    // Restart, hard reject, winding reversal, odd exit and post-quantization
    // degeneracy must not be approximated by an integer triangle-area test.
    mesh.strips[0].vertices[3].restart = true;
    Compare(mesh, {View()});
    mesh.strips[0].vertices.pop_back();
    Compare(mesh, {View()});
    auto reversed = View(); reversed.screen[0][0] *= -1;
    Compare(mesh, {reversed});
    auto rejected = View(); rejected.clip[3][0] = 4;
    Compare(mesh, {rejected});
    auto fractional = View(); fractional.screen[0][0] = .03125f;
    Compare(mesh, {fractional});
    std::vector<ps2r::gs::DrawIr> sentinel(1);
    mesh.strips[0].vertices[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK(G::Build(mesh, {View()}, sentinel) == G::Result::InvalidGeometry && sentinel.size() == 1);
    std::printf("ShadowGeometry: %d checks, %d failures (current NEW family, same source values)\n",
                checks, failures);
    return failures ? 1 : 0;
}
