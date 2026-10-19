#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_asset_oracle.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "engine/core/hash/hash.h"

namespace {
using G = title::ShadowGeometry;
size_t checks = 0, cases = 0, triangles = 0, fans = 0;
void Check(bool value, int line) {
    ++checks;
    if (!value) throw std::runtime_error("shadow asset case " + std::to_string(cases) +
                                         ", line " + std::to_string(line));
}
#define CHECK(value) Check(bool(value), __LINE__)
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.is_open());
    return {std::istreambuf_iterator<char>(file), {}};
}
G::View View(unsigned variant, float extent) {
    G::View view;
    const float angle = .37f * float(variant);
    const float c = std::cos(angle) / extent, s = std::sin(angle) / extent;
    view.clip = {{{c, 0, s, 0}, {0, 1 / extent, 0, 0}, {-s, 0, c, 0}, {0, 0, 0, 1}}};
    if (variant % 3 == 1) view.clip[2][3] = .2f / extent;
    if (variant % 3 == 2) view.clip[3][3] = .65f;
    if (variant >= 3) view.clip[3][(variant / 2) % 3] = variant & 1 ? -.5f : .5f;
    for (unsigned row = 0; row < 4; ++row) {
        view.screen[row][0] = view.clip[row][0] * 1024 + view.clip[row][3] * 32768;
        view.screen[row][1] = view.clip[row][1] * -896 + view.clip[row][3] * 32768;
        view.screen[row][2] = view.clip[row][2] * 8388607 - view.clip[row][3] * 8388607;
        view.screen[row][3] = view.clip[row][3];
    }
    view.guard = {1.1f, 1.1f, 1.1f, 1};
    view.offset = {1920, 1936};
    return view;
}
auto Corner(const ps2r::gs::VertexRec& v) { return std::make_tuple(v.x, v.y, v.z, v.r, v.g, v.b, v.a); }
void Compare(const ShadowAssetOracle& oracle, const std::vector<uint8_t>& bytes,
             const G::Mesh& mesh, const std::vector<G::View>& views) {
    ++cases;
    const auto original = oracle.Run(bytes, views);
    // The retail meshes' instances are eye-sized (NeedsEye 1): the original
    // program's output does not depend on the eye quadword or the stride.
    const auto eyed = oracle.Run(bytes, views, true);
    CHECK(eyed.size() == original.size());
    for (size_t group = 0; group < original.size(); ++group) {
        CHECK(eyed[group].prim_type == original[group].prim_type &&
              eyed[group].verts.size() == original[group].verts.size());
        for (size_t i = 0; i < original[group].verts.size(); ++i)
            CHECK(Corner(eyed[group].verts[i]) == Corner(original[group].verts[i]));
    }
    std::vector<ps2r::gs::DrawIr> selected;
    std::fesetround(FE_UPWARD);
    CHECK(G::Build(mesh, views, selected) == G::Result::Ok);
    CHECK(std::fegetround() == FE_UPWARD);
    std::fesetround(FE_TONEAREST);
    if (original.size() != selected.size())
        std::fprintf(stderr, "case %zu groups NEW=%zu selected=%zu\n",
                     cases, original.size(), selected.size());
    CHECK(original.size() == selected.size());
    for (size_t group = 0; group < original.size(); ++group) {
        const auto& a = original[group]; const auto& b = selected[group];
        CHECK(a.prim_type == b.prim_type && a.verts.size() == b.verts.size());
        for (size_t i = 0; i < a.verts.size(); ++i) {
            if (Corner(a.verts[i]) != Corner(b.verts[i]))
                std::fprintf(stderr, "group=%zu vertex=%zu NEW=%.9g,%.9g,%.9g/%u "
                             "selected=%.9g,%.9g,%.9g/%u\n", group, i,
                             a.verts[i].x, a.verts[i].y, a.verts[i].z, a.verts[i].r,
                             b.verts[i].x, b.verts[i].y, b.verts[i].z, b.verts[i].r);
            CHECK(Corner(a.verts[i]) == Corner(b.verts[i]));
        }
        triangles += a.verts.size() / 3;
        fans += a.prim_type == 5;
    }
}
void Refusals(const std::vector<uint8_t>& bytes, uint32_t vertices) {
    G::Mesh sentinel{{G::Strip{{{{12345, 0, 0}, true, true}}}}};
    const auto reject = [&](const uint8_t* data, size_t size, uint32_t count) {
        CHECK(G::Decode(data, size, count, sentinel) != G::Result::Ok);
        CHECK(sentinel.strips.size() == 1 && sentinel.strips[0].vertices.size() == 1 &&
              sentinel.strips[0].vertices[0].position[0] == 12345);
    };
    // Every prefix is refused, including header/normal/terminator truncation.
    // A complete header extent can omit only the source's trailing zero bytes.
    const size_t extent = 16 + size_t(bytes[0] | uint32_t(bytes[1]) << 8) * 16;
    for (size_t size = 0; size < extent; ++size) reject(bytes.data(), size, vertices);
    reject(nullptr, bytes.size(), vertices);
    reject(bytes.data(), bytes.size(), vertices + 1);
    reject(bytes.data(), bytes.size(), G::kMaxVertices + 1);
    auto corrupt = bytes;
    corrupt[16] ^= 1;
    reject(corrupt.data(), corrupt.size(), vertices);
    corrupt = bytes;
    // First strip's normals are a required ownership dependency, not optional.
    const uint32_t count = bytes[20] & 255;
    const size_t normal_header = 60 + size_t(count) * 12 + 4 + size_t(count) * 16;
    CHECK(normal_header + 4 < corrupt.size());
    corrupt[normal_header] ^= 1;
    reject(corrupt.data(), corrupt.size(), vertices);
}
}
int main(int argc, char** argv) {
    if (argc != 3) {
        std::puts("SKIP: provide audit_shadow_assets.py shadows.txt and original NTSC ELF");
        return 77;
    }
    try {
        const ShadowAssetOracle oracle(Read(argv[2]));
        const std::filesystem::path list = argv[1];
        std::ifstream rows(list);
        CHECK(rows.is_open());
        std::set<unsigned> shapes;
        size_t strips = 0, vertices_total = 0, reversed = 0;
        std::string row;
        while (std::getline(rows, row)) {
            std::istringstream fields(row);
            unsigned shape, vertices;
            uint64_t hash;
            std::string name, extra;
            CHECK(fields >> shape >> vertices >> std::hex >> hash >> name);
            CHECK(!(fields >> extra) && shape < 8 && shapes.insert(shape).second);
            CHECK(std::filesystem::path(name).filename() == name);
            auto bytes = Read(list.parent_path() / name);
            CHECK(ps2r::hash::Fnv1a64(bytes.data(), bytes.size()) == hash);
            G::Mesh mesh;
            CHECK(G::Decode(bytes.data(), bytes.size(), vertices, mesh) == G::Result::Ok);
            Refusals(bytes, vertices);
            float extent = 0;
            for (const auto& strip : mesh.strips) {
                ++strips;
                vertices_total += strip.vertices.size();
                for (const auto& vertex : strip.vertices) {
                    reversed += vertex.reverse_fan;
                    for (float value : vertex.position) extent = std::max(extent, std::abs(value));
                }
            }
            CHECK(extent > 0);
            for (unsigned variant = 0; variant < 12; ++variant)
                Compare(oracle, bytes, mesh, {View(variant, extent)});
            Compare(oracle, bytes, mesh, {View(3, extent), View(4, extent), View(5, extent)});
        }
        CHECK(shapes.size() == 8 && vertices_total == 538 && strips == 18);
        CHECK(reversed > 0 && triangles > 0 && fans > 0);
        std::printf("ShadowAssets: %zu checks, %zu cases, %zu triangles/%zu clipped fans; "
                    "all 8 original shapes/538 vertices, unchanged NEW VIF + original program\n",
                    checks, cases, triangles, fans);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
