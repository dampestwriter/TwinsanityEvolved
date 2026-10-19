#include "games/slus_20909_crash_twinsanity/tests/title/effects/cloth_oracle.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "engine/core/hash/hash.h"

namespace {
using C = title::ClothGeometry;
using G = title::MeshGeometry;
unsigned checks = 0, cases = 0, triangles = 0, fans = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("case ") + std::to_string(cases) + ", line " + std::to_string(__LINE__) + ": " + #value); } while (0)
uint32_t Bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
std::vector<uint8_t> Asset(unsigned count, bool restart) {
    std::vector<uint32_t> words{0, 0, 0x070000fa, 0,
        0x6c018000, count | 0x8000, 0x30024000, 0x512, 0,
        0x01000101, 0x64018001, count * 4, count | 0x8000,
        0x01000104, 0x68008003 | count << 16};
    for (unsigned i = 0; i < count; ++i) {
        words.push_back(Bits(float(i / 2) * .13f - .7f));
        words.push_back(Bits(i % 2 ? -.55f : .55f));
        words.push_back(Bits(float(i % 4) * .21f - .2f));
    }
    words.push_back(0x6c008004 | count << 16);
    for (unsigned i = 0; i < count; ++i) {
        words.push_back(0x3f000000 | (17 + i));
        words.push_back(0x3e800000 | (43 + i));
        words.push_back(0x3f800000 | (63 + i));
        words.push_back(127 | ((i < 2 || (restart && i == count / 2)) ? 0x8000 : 0));
    }
    words.push_back(0x14000000);
    words.push_back(0x01000101);
    while (words.size() % 4) words.push_back(0);
    words[0] = 0x60000000 | uint32_t(words.size() / 4 - 1);
    std::vector<uint8_t> bytes(words.size() * 4);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}
G::View View(unsigned variant) {
    G::View view;
    const float angle = .2f * float(variant);
    const float c = std::cos(angle), s = std::sin(angle);
    view.clip = {{{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}, {0, 0, 0, 1}}};
    if (variant % 3 == 1) view.clip[2][3] = .2f;
    if (variant % 3 == 2) view.clip[3][3] = .65f;
    if (variant >= 3) view.clip[3][(variant / 2) % 3] = variant & 1 ? -.7f : .7f;
    for (unsigned row = 0; row < 4; ++row) {
        view.screen[row][0] = view.clip[row][0] * 1024 + view.clip[row][3] * 32768;
        view.screen[row][1] = view.clip[row][1] * -896 + view.clip[row][3] * 32768;
        view.screen[row][2] = view.clip[row][2] * 8388607 + view.clip[row][3] * 8388607;
        view.screen[row][3] = view.clip[row][3];
    }
    view.guard = {1.1f, 1.1f, 1.1f, 1};
    view.scroll = {.125f, -.0625f};
    return view;
}
ps2r::gs::DrawIr Material() {
    ps2r::gs::DrawIr state;
    state.prim_type = 4;
    state.tex_enabled = true;
    state.off_x = 1920;
    state.off_y = 1936;
    return state;
}
auto Corner(const ps2r::gs::VertexRec& v) {
    return std::make_tuple(v.x, v.y, v.z, v.r, v.g, v.b, v.a, Bits(v.s), Bits(v.t), Bits(v.q));
}
void Compare(const ClothOracle& oracle, const std::vector<uint8_t>& bytes, unsigned count,
             const C::Waves& waves, const std::vector<G::View>& views) {
    ++cases;
    G::Mesh mesh;
    CHECK(G::Decode(bytes.data(), bytes.size(), count, mesh) == G::Result::Ok);
    const auto positions = mesh.strips[0].vertices[0].position;
    const auto original = oracle.Run(bytes, waves, views, Material());
    std::vector<ps2r::gs::DrawIr> replacement;
    std::fesetround(FE_UPWARD);
    CHECK(C::Build(mesh, waves, views, Material(), replacement) == C::Result::Ok);
    CHECK(std::fegetround() == FE_UPWARD);
    std::fesetround(FE_TONEAREST);
    CHECK(mesh.strips[0].vertices[0].position == positions);
    if (original.size() != replacement.size())
        std::fprintf(stderr, "cloth case %u groups NEW=%zu selected=%zu\n",
                     cases, original.size(), replacement.size());
    CHECK(original.size() == replacement.size());
    for (size_t group = 0; group < original.size(); ++group) {
        const auto& a = original[group]; const auto& b = replacement[group];
        CHECK(a.prim_type == b.prim_type && a.verts.size() == b.verts.size());
        for (size_t i = 0; i < a.verts.size(); ++i) {
            if (Corner(a.verts[i]) != Corner(b.verts[i]))
                std::fprintf(stderr, "group=%zu v=%zu NEW=%.9g,%.9g,%.9g rgba=%u,%u,%u,%u "
                    "stq=%08x,%08x,%08x selected=%.9g,%.9g,%.9g rgba=%u,%u,%u,%u stq=%08x,%08x,%08x\n",
                    group, i, a.verts[i].x, a.verts[i].y, a.verts[i].z,
                    a.verts[i].r, a.verts[i].g, a.verts[i].b, a.verts[i].a,
                    Bits(a.verts[i].s), Bits(a.verts[i].t), Bits(a.verts[i].q),
                    b.verts[i].x, b.verts[i].y, b.verts[i].z,
                    b.verts[i].r, b.verts[i].g, b.verts[i].b, b.verts[i].a,
                    Bits(b.verts[i].s), Bits(b.verts[i].t), Bits(b.verts[i].q));
            CHECK(Corner(a.verts[i]) == Corner(b.verts[i]));
        }
        triangles += unsigned(a.verts.size() / 3);
        fans += a.prim_type == 5;
    }
}
void Refusals() {
    const auto bytes = Asset(3, false);
    G::Mesh mesh;
    CHECK(G::Decode(bytes.data(), bytes.size(), 3, mesh) == G::Result::Ok);
    C::Waves waves;
    std::vector<ps2r::gs::DrawIr> output{Material()};
    output[0].src_frame = 12345;
    waves.amplitude[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(C::Build(mesh, waves, {View(0)}, Material(), output) == C::Result::InvalidGeometry);
    CHECK(output.size() == 1 && output[0].src_frame == 12345);
    waves = {};
    waves.kind = C::Kind(0x18);
    CHECK(C::Build(mesh, waves, {View(0)}, Material(), output) == C::Result::UnsupportedState);
    waves = {};
    mesh.strips[0].vertices[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK(C::Build(mesh, waves, {View(0)}, Material(), output) == C::Result::InvalidGeometry);
    CHECK(output[0].src_frame == 12345);
}
void Corpus(const ClothOracle& oracle, const std::filesystem::path& list) {
    std::ifstream rows(list);
    CHECK(rows.is_open());
    const std::string manifest{std::istreambuf_iterator<char>(rows), {}};
    CHECK(ps2r::hash::Fnv1a64(manifest.data(), manifest.size()) == 0x3dd46578c6d1a609ull);
    std::istringstream contents(manifest);
    std::set<std::string> seen;
    std::set<unsigned> families;
    size_t fixtures = 0, vertices_total = 0;
    std::string row;
    while (std::getline(contents, row)) {
        std::istringstream fields(row);
        unsigned kind, vertices;
        uint64_t hash;
        C::Waves waves;
        std::string filename, extra;
        CHECK(fields >> kind >> vertices >> std::hex >> hash >> std::dec >>
            waves.amplitude[0] >> waves.amplitude[1] >> waves.amplitude[2] >> filename);
        CHECK(!(fields >> extra) && (kind == 0x17 || kind == 0x1a) && seen.insert(row).second);
        CHECK(std::filesystem::path(filename).filename() == filename);
        std::ifstream file(list.parent_path() / filename, std::ios::binary);
        CHECK(file.is_open());
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        CHECK(ps2r::hash::Fnv1a64(bytes.data(), bytes.size()) == hash);
        G::Mesh mesh;
        CHECK(G::Decode(bytes.data(), bytes.size(), vertices, mesh) == G::Result::Ok);
        float extent = 0;
        for (const auto& strip : mesh.strips)
            for (const auto& vertex : strip.vertices)
                for (float value : vertex.position) extent = std::max(extent, std::abs(value));
        CHECK(extent > 0);
        waves.kind = C::Kind(kind);
        for (unsigned i = 0; i < 16; ++i)
            waves.rows[i] = {.25f - float(i) * .031f, float(i) * .037f - .2f, .7f - float(i) * .09f};
        const auto view = [&](unsigned variant) {
            auto result = View(variant);
            for (size_t row = 0; row < 3; ++row)
                for (size_t lane = 0; lane < 4; ++lane) {
                    result.clip[row][lane] /= extent;
                    result.screen[row][lane] /= extent;
                }
            return result;
        };
        for (unsigned variant = 0; variant < 12; ++variant)
            Compare(oracle, bytes, vertices, waves, {view(variant)});
        Compare(oracle, bytes, vertices, waves, {view(3), view(4), view(5)});
        ++fixtures;
        vertices_total += vertices;
        families.insert(kind);
    }
    CHECK(fixtures == 124 && vertices_total == 34353 &&
          families == (std::set<unsigned>{0x17, 0x1a}));
    std::printf("Complete cloth corpus: %zu original part/parameter fixtures, %zu vertices\n",
                fixtures, vertices_total);
}
}
int main(int argc, char** argv) {
    // With the ELF and manifest cache variables both unset the registered command
    // is "--assets" alone: that is a missing ELF, not a path to open.
    if ((argc != 2 && !(argc == 4 && std::string(argv[2]) == "--assets")) ||
        std::string(argv[1]).rfind("--", 0) == 0) {
        std::puts("SKIP: provide original verified NTSC ELF [--assets cloth.txt]");
        return 77;
    }
    try {
        std::ifstream file(argv[1], std::ios::binary);
        CHECK(file.is_open());
        const ClothOracle oracle({std::istreambuf_iterator<char>(file), {}});
        for (const auto kind : {C::Kind::Single, C::Kind::Three}) {
            C::Waves waves;
            waves.kind = kind;
            for (unsigned row = 0; row < 16; ++row)
                waves.rows[row] = {float(row) * .083f - .6f, .23f - float(row) * .07f,
                                  float(row % 3) * .31f - .5f};
            for (unsigned count : {3u, 4u, 5u, 17u, 38u}) {
                const auto bytes = Asset(count, count & 1);
                for (unsigned variant = 0; variant < 12; ++variant) {
                    waves.amplitude = variant % 3 ? std::array<float, 3>{.093f, -.07f, .15f} :
                                                   std::array<float, 3>{0, 0, 0};
                    Compare(oracle, bytes, count, waves, {View(variant)});
                }
                Compare(oracle, bytes, count, waves, {View(3), View(4), View(5)});
            }
        }
        Refusals();
        if (argc == 4) Corpus(oracle, argv[3]);
        CHECK(triangles > 0 && fans > 0);
        std::printf("ClothGeometry: %u checks/%u cases, %u triangles/%u fans; original complete 17/1A programs\n",
                    checks, cases, triangles, fans);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
