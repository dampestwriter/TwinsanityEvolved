#include "games/slus_20909_crash_twinsanity/tests/title/effects/cloth_material_oracle.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "games/slus_20909_crash_twinsanity/tests/recomp/strip_oracle_memory.h"
#include "engine/core/hash/hash.h"

namespace {
using C = title::ClothMaterial;
using G = title::ClothGeometry;
namespace ram = strip_oracle::memory;
unsigned checks = 0, cases = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("cloth material case ") + std::to_string(cases) + ", line " + \
    std::to_string(__LINE__) + ": " + #value); } while (0)
constexpr uint64_t Base = uint64_t(7) << 26 | uint64_t(2) << 21 | uint64_t(1) << 55;
auto Lookup(const ClothMaterialOracle& oracle) {
    return [&oracle](uint32_t address, title::MeshMaterial::Texture& texture) {
        if (address != ClothMaterialOracle::kTexture) return false;
        texture = oracle.texture;
        return true;
    };
}
void Same(const ps2r::gs::DrawIr& a, const ps2r::gs::DrawIr& b) {
    const auto ar = a.ToDrawRec(), br = b.ToDrawRec();
    if (std::memcmp(&ar, &br, sizeof(ar))) {
        const auto* ab = reinterpret_cast<const uint8_t*>(&ar);
        const auto* bb = reinterpret_cast<const uint8_t*>(&br);
        for (size_t i = 0; i < sizeof(ar); ++i)
            if (ab[i] != bb[i]) std::fprintf(stderr, "record byte %zu: NEW=%02x selected=%02x\n", i, ab[i], bb[i]);
    }
    CHECK(std::memcmp(&ar, &br, sizeof(ar)) == 0);
    CHECK(a.raster.dthe == b.raster.dthe && a.raster.dimx == b.raster.dimx);
    CHECK(std::tie(a.tex.mmag, a.tex.mmin, a.tex.mxl, a.tex.lcm, a.tex.l, a.tex.k) ==
          std::tie(b.tex.mmag, b.tex.mmin, b.tex.mxl, b.tex.lcm, b.tex.l, b.tex.k));
}
void Compare(ClothMaterialOracle& oracle, G::Kind kind, uint64_t settings,
             int16_t k, int16_t l, unsigned levels, unsigned variant,
             std::array<float, 3> amplitude = {.125f, -.0625f, .1875f},
             unsigned bucket = ~0u) {
    ++cases;
    oracle.Reset(kind, settings, k, l, levels, variant);
    if (bucket == ~0u) bucket = variant % 2 ? 9u : 2u;
    ram::write(ClothMaterialOracle::kMaterial + 0x20, bucket);
    ram::write(ClothMaterialOracle::kShader + 0x27c, amplitude);
    C::Output replacement;
    const auto before = ram::read<std::array<uint8_t, 0x290>>(ClothMaterialOracle::kShader);
    const auto captured = C::Capture(oracle.Reader(), ClothMaterialOracle::kMaterial, oracle.environment,
                                     Lookup(oracle), replacement);
    if (captured != C::Result::Ok) std::fprintf(stderr, "capture refused: %d\n", int(captured));
    CHECK(captured == C::Result::Ok);
    CHECK((ram::read<std::array<uint8_t, 0x290>>(ClothMaterialOracle::kShader) == before));
    Same(oracle.Original(), replacement.material.state);
    CHECK(replacement.material.bucket == bucket);
    CHECK(replacement.material.texture.lease == oracle.texture.lease);
    CHECK((replacement.material.scroll == std::array<float, 2>{.125f, -.375f}));
    CHECK(replacement.waves.kind == kind);
    CHECK(replacement.waves.amplitude[0] == amplitude[0]);
    CHECK(replacement.waves.amplitude[1] == (kind == G::Kind::Three ? amplitude[1] : 0));
    CHECK(replacement.waves.amplitude[2] == (kind == G::Kind::Three ? amplitude[2] : 0));
    for (unsigned row = 0; row < 16; ++row) {
        CHECK((replacement.waves.rows[row] == std::array<float, 3>{float(row) * .1f - .75f, .5f, -.8f}));
        ram::write(ClothMaterialOracle::kWaves + row * 16, std::array<float, 4>{42, 43, 44, 45});
        CHECK(replacement.waves.rows[row][0] == float(row) * .1f - .75f);
    }
    // OLD BUG-010/082: source storage reuse cannot repaint a queued material.
    const std::weak_ptr<const title::Graphics::Resource> generation = oracle.texture.lease;
    oracle.texture = {};
    CHECK(!generation.expired() && replacement.material.texture.lease);
}
void Refusals(ClothMaterialOracle& oracle) {
    oracle.Reset(G::Kind::Three, Base, -69, 0, 3, 1);
    C::Output out;
    out.material.bucket = 12345;
    out.waves.amplitude[2] = 54321;
    const auto refuse = [&](const title::MaterialState::Read& read, C::Result expected) {
        CHECK(C::Capture(read, ClothMaterialOracle::kMaterial, oracle.environment, Lookup(oracle), out) == expected);
        CHECK(out.material.bucket == 12345 && out.waves.amplitude[2] == 54321);
    };
    for (uint32_t at : {ClothMaterialOracle::kMaterial, ClothMaterialOracle::kMaterial + 0x60,
            ClothMaterialOracle::kShader, ClothMaterialOracle::kShader + 0x270,
            ClothMaterialOracle::kShader + 0x284, ClothMaterialOracle::kWaves + 255,
            C::kThreeVtable + 0x2c, ClothMaterialOracle::kTarget + 31,
            title::MaterialState::kDisplayWidth})
        refuse(oracle.Reader(at), C::Result::InvalidMemory);
    refuse([](uint32_t, void*, size_t) -> bool { throw std::runtime_error("read failure"); },
           C::Result::InvalidMemory);
    ram::write<uint32_t>(ClothMaterialOracle::kShader + 0x64, 1);
    refuse(oracle.Reader(), C::Result::UnsupportedMaterial);
    ram::write<uint32_t>(ClothMaterialOracle::kShader + 0x64, 0x1a);
    ram::write<uint32_t>(C::kThreeVtable + 0x2c, 0x1d18c8);
    refuse(oracle.Reader(), C::Result::UnsupportedMaterial);
    ram::write<uint32_t>(C::kThreeVtable + 0x2c, 0x1d0d20);
    ram::write<float>(ClothMaterialOracle::kWaves, std::numeric_limits<float>::quiet_NaN());
    refuse(oracle.Reader(), C::Result::UnsupportedMaterial);
    ram::write<float>(ClothMaterialOracle::kWaves, 0);
    ram::write<uint32_t>(ClothMaterialOracle::kShader + 0x270, 0x1ffffff0);
    refuse(oracle.Reader(), C::Result::InvalidMemory);
    ram::write<uint32_t>(ClothMaterialOracle::kShader + 0x270, ClothMaterialOracle::kWaves);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes, 4);
    refuse(oracle.Reader(), C::Result::UnsupportedTarget);
    ram::write<uint32_t>(title::MaterialState::kDisplayPixelBytes, 2);
    oracle.texture.lease.reset();
    refuse(oracle.Reader(), C::Result::MissingTexture);
    CHECK(C::Capture(oracle.Reader(), 0xffffffff, oracle.environment, Lookup(oracle), out) ==
          C::Result::InvalidMemory);
}
template<class T> T Field(const std::vector<uint8_t>& bytes, size_t at) {
    CHECK(at <= bytes.size() && sizeof(T) <= bytes.size() - at);
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}
void Corpus(ClothMaterialOracle& oracle, const std::filesystem::path& list) {
    std::ifstream stream(list);
    CHECK(stream.is_open());
    const std::string manifest{std::istreambuf_iterator<char>(stream), {}};
    CHECK(ps2r::hash::Fnv1a64(manifest.data(), manifest.size()) == 0xf35791fb4c4c09f7ull);
    std::istringstream lines(manifest);
    std::string line;
    std::set<uint32_t> ids;
    size_t singles = 0, triples = 0;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        uint32_t id;
        uint64_t hash;
        std::string filename, extra;
        CHECK(fields >> std::hex >> id >> hash >> filename);
        CHECK(!(fields >> extra) && ids.insert(id).second &&
              std::filesystem::path(filename).filename() == filename);
        std::ifstream file(list.parent_path() / filename, std::ios::binary);
        CHECK(file.is_open());
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        CHECK(ps2r::hash::Fnv1a64(bytes.data(), bytes.size()) == hash);
        const size_t name = Field<uint32_t>(bytes, 12);
        CHECK(Field<uint32_t>(bytes, 16 + name) == 1);
        const uint32_t kind = Field<uint32_t>(bytes, 20 + name);
        CHECK(kind == 0x17 || kind == 0x1a);
        const bool three = kind == 0x1a;
        const size_t parameters = 24 + name, settings_at = parameters + (three ? 20 : 12);
        CHECK(bytes.size() == settings_at + 90 && Field<uint32_t>(bytes, settings_at + 86) == kind);
        CHECK(Field<uint32_t>(bytes, parameters) == 0); // complete authored mode inventory
        std::array<float, 3> amplitudes{Field<float>(bytes, parameters + 8), 0, 0};
        if (three) {
            amplitudes[1] = Field<float>(bytes, parameters + 12);
            amplitudes[2] = Field<float>(bytes, parameters + 16);
        }
        constexpr unsigned shifts[]{0,1,5,6,9,17,19,20,21,23,26,27,28,29,30,
                                    32,35,38,39,41,43,45,47,55,56,57,58,59,60,61};
        uint64_t settings = 0;
        for (size_t i = 0; i < 30; ++i) settings |= uint64_t(bytes[settings_at + i]) << shifts[i];
        const auto k = Field<int16_t>(bytes, settings_at + 30), l = Field<int16_t>(bytes, settings_at + 32);
        for (unsigned levels : {1u, 3u, 7u})
            Compare(oracle, G::Kind(kind), settings, k, l, levels, unsigned(ids.size()),
                    amplitudes, Field<uint32_t>(bytes, 8));
        if (three) ++triples; else ++singles;
    }
    CHECK(ids.size() == 50 && singles == 46 && triples == 4);
    std::printf("Original cloth materials: %zu records, 46 single/4 three-amplitude\n", ids.size());
}
}
int main(int argc, char** argv) {
    // With the ELF and manifest cache variables both unset the registered command
    // is "--assets" alone: that is a missing ELF, not a path to open.
    if ((argc != 2 && !(argc == 4 && std::string(argv[2]) == "--assets")) ||
        std::string(argv[1]).rfind("--", 0) == 0) {
        std::puts("SKIP: provide original NTSC ELF [--assets cloth-materials.txt]");
        return 77;
    }
    try {
        std::ifstream file(argv[1], std::ios::binary);
        CHECK(file.is_open());
        ClothMaterialOracle oracle({std::istreambuf_iterator<char>(file), {}});
        for (const auto kind : {G::Kind::Single, G::Kind::Three}) {
            for (unsigned preset = 0; preset < 16; ++preset) {
                const uint64_t settings = Base | 1 | uint64_t(preset) << 1 |
                    uint64_t(preset & 7) << 6 | uint64_t(preset * 17) << 9 |
                    uint64_t(preset & 3) << 17 | uint64_t(preset & 1) << 56 |
                    uint64_t((preset >> 1) & 1) << 59 | uint64_t(preset & 1) << 5;
                Compare(oracle, kind, settings, int16_t(preset * 17 - 69), 0, preset % 7 + 1, preset);
            }
            for (unsigned selectors = 0; selectors < 16; ++selectors) {
                const uint64_t settings = Base | 1 | uint64_t(1) << 38 |
                    uint64_t(selectors % 3) << 39 | uint64_t((selectors / 2) % 3) << 41 |
                    uint64_t((selectors / 4) % 3) << 43 | uint64_t((selectors / 3) % 3) << 45 |
                    uint64_t(selectors * 17) << 47;
                // BUG-084's negative L and signed K must match the source's
                // overlapping shift, not a sanitized sampler approximation.
                Compare(oracle, kind, settings, int16_t(selectors & 1 ? -32768 : 32767),
                        int16_t(selectors % 5 - 2), 3, selectors);
            }
        }
        Refusals(oracle);
        if (argc == 4) Corpus(oracle, argv[3]);
        CHECK(oracle.Calls() > cases * 90);
        std::printf("ClothMaterial: %u checks/%u cases, %llu complete generated body calls; "
                    "no active material differences\n", checks, cases,
                    static_cast<unsigned long long>(oracle.Calls()));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
