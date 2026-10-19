// Authored assets -> actual NEW VIF + static retail shader -> DrawIR, compared
// with owned geometry -> SkyGeometry. No captured runtime images or PCSX2 data.
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#include "engine/core/hash/hash.h"
#include "engine/gif/gif_arbiter.h"
#include "engine/gif/gif_parser.h"
#include "engine/gif/hle/gs_state_sink.h"
#include "engine/gs/state/gs_state.h"
#include "engine/vif/vif.h"
#include "engine/vu/tiers/vu_tier_dispatch.h"
#include "engine/vu/vu_unit.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_geometry.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace {
using Geometry = title::SkyGeometry;
using Draw = ps2r::gs::DrawIr;
size_t cases = 0, compared = 0, fans = 0, rejection_checks = 0;
std::ofstream comparison;
[[noreturn]] void Fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL case %zu line %d: %s\n", cases, line, expression);
    std::exit(1);
}
#define REQUIRE(value) do { if (!(value)) Fail(#value, __LINE__); } while (false)
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
template<class T> T Load(const std::vector<uint8_t>& data, size_t offset) {
    REQUIRE(offset <= data.size() && sizeof(T) <= data.size() - offset);
    T result;
    std::memcpy(&result, data.data() + offset, sizeof(result));
    return result;
}
std::vector<uint8_t> Elf(const std::vector<uint8_t>& elf, uint32_t address, size_t size) {
    const auto offset = Load<uint32_t>(elf, 28);
    const auto stride = Load<uint16_t>(elf, 42), count = Load<uint16_t>(elf, 44);
    for (size_t i = 0; i != count; ++i) {
        const size_t at = offset + i * stride;
        if (Load<uint32_t>(elf, at) != 1) continue;
        const auto start = Load<uint32_t>(elf, at + 8), length = Load<uint32_t>(elf, at + 16);
        if (address < start || size > length || address - start > length - size) continue;
        const size_t pos = Load<uint32_t>(elf, at + 4) + address - start;
        REQUIRE(pos <= elf.size() && size <= elf.size() - pos);
        return {elf.begin() + pos, elf.begin() + pos + size};
    }
    Fail("ELF address absent", __LINE__);
}
struct Sink : ps2r::gs::DrawIrSink {
    std::vector<Draw> draws;
    void OnDraw(const Draw& draw) override { draws.push_back(draw); }
    void OnJob(const ps2r::gs::JobRec&) override {}
    void OnFrame(const ps2r::gs::JobRec&) override {}
};
struct Rig {
    Sink sink;
    ps2r::gs::GsState gs;
    GsStateSink gs_sink{gs};
    GifRegs regs;
    GifParser parser{gs_sink, regs};
    GifArbiter arbiter{parser};
    VuUnit vu0{0}, vu1{1, &arbiter};
    Vif vif{vu0, vu1, arbiter, regs};
    Rig(const std::vector<uint8_t>& elf, const Geometry::View& view) {
        gs.SetSink(&sink);
        // Source-owned test material and target. Full source producer/material
        // extraction is a separate gate; this test closes the static shader.
        gs.WriteRegister(0x1a, 0); // attributes from PRMODE
        gs.WriteRegister(0x1b, 0x18); // Gouraud, STQ textured
        gs.WriteRegister(0x4c, uint64_t(4) << 16);
        gs.WriteRegister(0x4e, uint64_t(0x30) << 24);
        gs.WriteRegister(0x18, uint64_t(1920 * 16) | uint64_t(1936 * 16) << 32);
        gs.WriteRegister(0x40, uint64_t(255) << 16 | uint64_t(223) << 48);
        gs.WriteRegister(0x47, uint64_t(1) << 16 | uint64_t(1) << 17);
        gs.WriteRegister(0x46, 1);
        gs.WriteRegister(0x06, uint64_t(2) << 14 | uint64_t(7) << 26 | uint64_t(6) << 30);
        for (const auto& program : std::array<std::array<uint32_t, 3>, 5>{
                 {{{0x2dad60, 18, 0}}, {{0x2d9ce0, 4, 0x90}}, {{0x2e40b0, 114, 0xb0}},
                  {{0x2e4440, 222, 0x440}}, {{0x2e3b40, 174, 0x1720}}}}) {
            const auto bytes = Elf(elf, program[0], program[1] * 8);
            vu1.WriteMicroMem(program[2], bytes.data(), bytes.size());
        }
        vu1.dispatch().settings().vu1_mode = VuTierMode::Interp;
        // Source dispatcher/node/matrix values, independent of asset decoding.
        Qw(1, {600, 750, 600, 750});
        Qw(3, {96, 0, 0, 0});
        Qw(4, {0, 0xb0 / 8, 0, 0});
        Qw(5, {0, 0x440 / 8, 0, 0});
        Qw(6, {0x8000, 0x3002c000, 0x512, 0});
        Qw(96, {0x1720 / 8, (0x1720 + 83 * 8) / 8, 112, 0});
        Qw(112, {0x90 / 8, 0, 112, 0});
        const std::array<float, 4> scroll{view.scroll[0], view.scroll[1], 0, 0};
        vu1.WriteDataMem(98 * 16, reinterpret_cast<const uint8_t*>(scroll.data()), 16);
        vu1.WriteDataMem(174 * 16, reinterpret_cast<const uint8_t*>(view.screen.data()), 64);
        vu1.WriteDataMem(178 * 16, reinterpret_cast<const uint8_t*>(view.clip.data()), 64);
        vu1.WriteDataMem(182 * 16, reinterpret_cast<const uint8_t*>(view.guard.data()), 16);
        vif.regs(1).base = vif.regs(1).tops = 256;
        vif.regs(1).ofst = 0;
    }
    void Qw(uint32_t address, std::array<uint32_t, 4> words) {
        vu1.WriteDataMem(address * 16, reinterpret_cast<const uint8_t*>(words.data()), 16);
    }
    void Run(const std::vector<uint8_t>& asset) {
        const size_t size = size_t(Load<uint16_t>(asset, 0)) * 16;
        REQUIRE(size <= asset.size() - 16);
        // Actual authored command bytes pass through the unchanged NEW model.
        vif.ProcessData(1, asset.data() + 16, size);
        REQUIRE(vif.IsIdle(1));
        arbiter.drain();
        gs.KickBoundary();
    }
};
Geometry::View View(unsigned variant) {
    Geometry::View view{};
    const float angle = float(variant) * 0.37f;
    const float c = std::cos(angle) * 0.013f, s = std::sin(angle) * 0.013f;
    view.clip = {{{c, 0, s, 0}, {0, 0.014f, 0, 0}, {-s, 0, c, 0}, {0, 0, 0, 1}}};
    if (variant % 3 == 1) view.clip[2][3] = 0.003f;
    if (variant % 3 == 2) view.clip[3][3] = 0.4f;
    for (unsigned row = 0; row != 4; ++row) {
        view.screen[row][0] = view.clip[row][0] * 1024 + view.clip[row][3] * 32768;
        view.screen[row][1] = view.clip[row][1] * -896 + view.clip[row][3] * 32768;
        view.screen[row][2] = view.clip[row][2] * -8388607 + view.clip[row][3] * 8388607;
        view.screen[row][3] = view.clip[row][3];
    }
    view.guard = {1.1f, 1.1f, 1.1f, 1};
    view.scroll = {0.11f * float(variant), -0.19f * float(variant)};
    return view;
}
void RejectMalformed(const std::vector<uint8_t>& asset, uint32_t vertices) {
    Geometry::Mesh decoded;
    REQUIRE(Geometry::Decode(asset.data(), asset.size(), vertices, decoded) == Geometry::Result::Ok);
    const auto reject_asset = [&](const uint8_t* bytes, size_t size, uint32_t declared) {
        Geometry::Mesh destination;
        destination.strips.resize(1);
        destination.strips[0].vertices.resize(1);
        destination.strips[0].vertices[0].position[0] = 12345;
        REQUIRE(Geometry::Decode(bytes, size, declared, destination) != Geometry::Result::Ok);
        REQUIRE(destination.strips.size() == 1 && destination.strips[0].vertices.size() == 1 &&
                destination.strips[0].vertices[0].position[0] == 12345);
        ++rejection_checks;
    };
    const size_t payload_end = 16 + size_t(Load<uint16_t>(asset, 0)) * 16;
    for (size_t cut : {size_t(0), size_t(15), size_t(16), payload_end - 1})
        reject_asset(asset.data(), cut, vertices);
    reject_asset(nullptr, asset.size(), vertices);
    reject_asset(asset.data(), asset.size(), vertices + 1);
    reject_asset(asset.data(), asset.size(), Geometry::kMaxVertices + 1);
    auto corrupt = asset;
    corrupt[16] ^= 1;
    reject_asset(corrupt.data(), corrupt.size(), vertices);

    Draw state;
    state.prim_type = 4; state.gouraud = state.tex_enabled = true;
    const auto reject_geometry = [&](const Geometry::Mesh& mesh, const Geometry::View& view,
                                     const Draw& material) {
        std::vector<Draw> destination(1);
        destination[0].seq = 12345;
        destination[0].verts.resize(1);
        destination[0].verts[0].x = 6789;
        const int rounding = std::fegetround();
        REQUIRE(Geometry::Build(mesh, view, material, destination) != Geometry::Result::Ok);
        REQUIRE(std::fegetround() == rounding);
        REQUIRE(destination.size() == 1 && destination[0].seq == 12345 &&
                destination[0].verts.size() == 1 && destination[0].verts[0].x == 6789);
        ++rejection_checks;
    };
    auto mesh = decoded;
    mesh.strips.back().vertices.resize(2);
    reject_geometry(mesh, View(0), state);
    mesh = decoded;
    mesh.strips.back().vertices.resize(39);
    reject_geometry(mesh, View(0), state);
    mesh = decoded;
    mesh.strips.back().vertices.back().position[1] = std::numeric_limits<float>::quiet_NaN();
    reject_geometry(mesh, View(0), state);
    mesh = decoded;
    mesh.strips.back().vertices.back().stq[2] = std::numeric_limits<float>::infinity();
    reject_geometry(mesh, View(0), state);
    auto view = View(0);
    view.screen[2][3] = std::numeric_limits<float>::infinity();
    reject_geometry(decoded, view, state);
    view = View(0);
    view.scroll[0] = std::numeric_limits<float>::quiet_NaN();
    reject_geometry(decoded, view, state);
    state.fog = true;
    reject_geometry(decoded, View(0), state);
}
void Compare(const std::vector<uint8_t>& elf, const std::vector<uint8_t>& asset,
             uint32_t vertices, const Geometry::View& view, int rounding) {
    std::fesetround(FE_TOWARDZERO);
    auto model = std::make_unique<Rig>(elf, view);
    model->Run(asset);
    Geometry::Mesh geometry;
    REQUIRE(Geometry::Decode(asset.data(), asset.size(), vertices, geometry) == Geometry::Result::Ok);
    Draw state;
    state.prim_type = 4; state.gouraud = state.tex_enabled = true;
    state.off_x = 1920; state.off_y = 1936;
    std::vector<Draw> selected;
    std::fesetround(rounding);
    REQUIRE(Geometry::Build(geometry, view, state, selected) == Geometry::Result::Ok);
    REQUIRE(std::fegetround() == rounding);
    auto& expected = model->sink.draws;
    if (expected.size() != selected.size())
        std::fprintf(stderr, "draw count model=%zu selected=%zu\n", expected.size(), selected.size());
    REQUIRE(expected.size() == selected.size());
    for (size_t i = 0; i != expected.size(); ++i) {
        const auto& a = expected[i]; const auto& b = selected[i];
        if (a.verts.size() != b.verts.size())
            std::fprintf(stderr, "draw%zu type %u/%u vertices %zu/%zu\n", i, a.prim_type, b.prim_type,
                         a.verts.size(), b.verts.size());
        REQUIRE(a.prim_type == b.prim_type && a.verts.size() == b.verts.size());
        for (size_t vertex = 0; vertex != a.verts.size(); ++vertex) {
            if (std::memcmp(&a.verts[vertex], &b.verts[vertex], sizeof(a.verts[vertex]))) {
                const auto* x = reinterpret_cast<const uint8_t*>(&a.verts[vertex]);
                const auto* y = reinterpret_cast<const uint8_t*>(&b.verts[vertex]);
                std::fprintf(stderr, "draw%zu vertex%zu:", i, vertex);
                for (size_t byte = 0; byte != sizeof(a.verts[vertex]); ++byte)
                    if (x[byte] != y[byte]) std::fprintf(stderr, " [%zu] %02x/%02x", byte, x[byte], y[byte]);
                std::fprintf(stderr, "\n");
                Fail("vertex bytes differ", __LINE__);
            }
        }
        compared += a.verts.size();
        fans += a.prim_type == 5;
        if (comparison) comparison << cases << '\t' << i << '\t' << unsigned(a.prim_type) << '\t'
            << a.verts.size() << '\t' << std::hex << a.snapshot.vertex_hash << '\t'
            << b.snapshot.vertex_hash << std::dec << '\n';
    }
    ++cases;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "need ELF and static asset directory\n"); return 77; }
    const auto elf = Read(argv[1]);
    if (elf.empty() || !std::filesystem::is_directory(argv[2])) return 77;
    if (argc > 3) comparison.open(argv[3]);
    const int saved = std::fegetround();
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(argv[2]))
        if (entry.path().filename().string().find("model_") == 0) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    REQUIRE(!paths.empty());
    constexpr int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    for (const auto& path : paths) {
        const auto model = Read(path);
        const uint32_t parts = Load<uint32_t>(model, 0);
        size_t offset = 4;
        for (uint32_t part = 0; part != parts; ++part) {
            const auto vertices = Load<uint32_t>(model, offset), size = Load<uint32_t>(model, offset + 4);
            offset += 8;
            REQUIRE(offset <= model.size() && size <= model.size() - offset);
            const std::vector<uint8_t> asset(model.begin() + offset, model.begin() + offset + size);
            offset += size;
            const uint32_t extra = Load<uint32_t>(model, offset);
            offset += 4 + extra;
            RejectMalformed(asset, vertices);
            for (unsigned variant = 0; variant != 8; ++variant)
                Compare(elf, asset, vertices, View(variant), modes[variant % 4]);
        }
        REQUIRE(offset == model.size());
    }
    std::fesetround(saved);
    std::printf("Sky static shader oracle: %zu cases, %zu vertices, %zu fans; zero differences; "
                "%zu atomic rejection checks\n", cases, compared, fans, rejection_checks);
}
