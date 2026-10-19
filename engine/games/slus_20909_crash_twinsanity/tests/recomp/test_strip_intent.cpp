#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_intent.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using Intent = title::StripIntent;
using Material = title::MaterialState;
int checks = 0, failures = 0;
void Check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %d: %s\n", line, expression); }
}
#define CHECK(value) Check((value), #value, __LINE__)

struct Fixture {
    static constexpr uint32_t object = 0x1000, positions = 0x2000, colors = 0x3000;
    static constexpr uint32_t shader = 0x4000, target = 0x5000, matrix = 0x6000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x310000, 0x83);
    std::vector<uint32_t> reads;
    uint32_t denied = 0;
    Material::Environment environment{true, false, 0, 0, 0, 0, 0, 0};
    void Put(uint32_t address, uint64_t value, size_t count = 4) {
        for (size_t i = 0; i < count; ++i) bytes.at(address + i) = uint8_t(value >> (i * 8));
    }
    void Float(uint32_t address, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        Put(address, bits);
    }
    Intent::Read Reader() {
        return [this](uint32_t address, void* out, size_t size) {
            reads.push_back(address);
            if (address >= bytes.size() || size > bytes.size() - address ||
                (denied && address <= denied && uint64_t(address) + size > denied))
                return false;
            std::memcpy(out, bytes.data() + address, size);
            return true;
        };
    }
    Fixture() {
        Put(object, Material::kFlatMaterial);
        Put(object + 0x10, 4, 1);  // next three bytes are intentionally dirty
        Put(object + 0x14, positions); Put(object + 0x18, colors);
        for (size_t i = 0; i < 4; ++i) {
            Float(positions + uint32_t(i * 8), float(i / 2) * 8);
            Float(positions + uint32_t(i * 8 + 4), float(i % 2) * 4);
            Put(colors + uint32_t(i * 4), 0x80c0c0c0);
        }
        Put(Material::kFlatMaterial, shader);
        Put(Material::kFlatMaterial + 0x10, 1);
        Put(Material::kFlatMaterial + 0x18, 2, 8);
        Put(Material::kFlatMaterial + 0x20, 24);
        Put(shader, 0x0880000004400001ull, 8);
        Put(shader + 0x64, 14); Put(shader + 0x6c, Material::kFlatShaderVtable);
        Put(Material::kRenderTarget, target);
        Put(target + 4, 0); Put(target + 8, 0);
        Put(target + 0xc, 512); Put(target + 0x10, 448);
        Put(target + 0x18, 512); Put(target + 0x1c, 448);
        Put(Material::kDisplayWidth, 512); Put(Material::kDisplayHeight, 448);
        Put(Material::kDisplayPixelBytes, 2); Put(Material::kDepthBufferPage, 168);
        for (uint32_t i = 0; i < 16; ++i) Float(matrix + i * 4, i % 5 == 0 ? 1 : 0);
        Float(matrix + 48, 5); Float(matrix + 52, 7);
    }
    Intent::Result Capture(uint32_t entry, uint32_t a1, uint32_t a2, Intent::Output& out,
                           uint32_t a0 = object) {
        return Intent::Capture(Reader(), entry, a0, a1, a2, environment, out);
    }
};

Intent::Output Seed() {
    Intent::Output out;
    out.bucket = 19; out.draw.seq = 771;
    out.draw.vertex_count = 7; out.draw.verts.resize(7);
    out.draw.verts[0].x = 3.25f;
    return out;
}
void Unchanged(const Intent::Output& out) {
    CHECK(out.bucket == 19 && out.draw.seq == 771);
    CHECK(out.draw.vertex_count == 7 && out.draw.verts.size() == 7);
    CHECK(out.draw.verts[0].x == 3.25f);
}
uint32_t Color(const ps2r::gs::VertexRec& vertex) {
    return vertex.r | uint32_t(vertex.g) << 8 | uint32_t(vertex.b) << 16 |
           uint32_t(vertex.a) << 24;
}

void TestAllEntriesAndOwnedState() {
    Fixture f;
    const auto before = f.bytes;
    const std::array<uint32_t, 4> entries{Intent::kPlainAddress, Intent::kTintedAddress,
                                        Intent::kPlacedAddress, Intent::kPlacedTintedAddress};
    for (size_t i = 0; i < entries.size(); ++i) {
        Intent::Output out;
        const bool placed = i >= 2, tinted = (i % 2) != 0;
        CHECK(f.Capture(entries[i], placed ? Fixture::matrix : 0x40606060,
                        0x40606060, out) == Intent::Result::Ok);
        CHECK(out.bucket == 24 && out.draw.verts.size() == 6);
        CHECK(out.draw.verts[0].x == (placed ? 5 : 0));
        CHECK(out.draw.verts[0].y == (placed ? 7 : 0));
        CHECK(Color(out.draw.verts[0]) == (tinted ? 0x40606060u : 0x80c0c0c0u));
        CHECK(out.draw.raster.frame.fbp == 0x70000 && out.draw.raster.frame.fbw == 512);
        CHECK(out.draw.raster.zbuf.zmsk && out.draw.raster.test.ztst == 2);
        CHECK(out.draw.raster.alpha.a == 0 && out.draw.raster.alpha.b == 1 &&
              out.draw.raster.alpha.c == 0 && out.draw.raster.alpha.d == 1);
        CHECK(out.draw.raster.fba && out.draw.raster.colclamp && !out.draw.raster.pabe);
        CHECK(!out.draw.fog && !out.draw.tex_enabled);
        CHECK(out.draw.verts[0].z == 4294967040.0f);
        CHECK(out.draw.seq == 0 && out.draw.identity.hle_handler == 0);
        CHECK(f.bytes == before);
        f.bytes.assign(f.bytes.size(), 0);
        CHECK(out.draw.verts[0].x == (placed ? 5 : 0));
        CHECK(Color(out.draw.verts[0]) == (tinted ? 0x40606060u : 0x80c0c0c0u));
        f.bytes = before;
    }
}

void TestRangesEntriesAndAtomicDecline() {
    Fixture f;
    for (uint32_t entry : {0u, Intent::kPlainAddress + 4, 0x001a9a30u}) {
        auto out = Seed();
        f.reads.clear();
        CHECK(f.Capture(entry, 0, 0, out) == Intent::Result::UnsupportedEntry);
        CHECK(f.reads.empty());
        Unchanged(out);
    }
    for (uint32_t pointer : {0u, 0x1001u, 0x02000000u, 0x70000000u, 0x10000000u,
                             0xfffffff0u, 0x01fffff0u}) {
        auto out = Seed();
        CHECK(f.Capture(Intent::kPlainAddress, 0, 0, out, pointer) ==
              Intent::Result::InvalidMemory);
        Unchanged(out);
    }
    for (uint32_t count : {0u, 1u, 2u}) {
        f.Put(Fixture::object + 0x10, count, 1);
        auto out = Seed();
        f.reads.clear();
        CHECK(f.Capture(Intent::kPlacedAddress, Fixture::matrix, 0, out) == Intent::Result::Empty);
        CHECK(f.reads.size() > 1); // material/target and source inputs still validated
        Unchanged(out);
        if (count) {
            CHECK(f.Capture(Intent::kPlacedAddress,0,0,out) == Intent::Result::InvalidMemory);
            Unchanged(out);
        }
    }
    f.Put(Fixture::object + 0x10, 4, 1);
    for (uint32_t address : {Fixture::object, Fixture::positions, Fixture::colors,
                             Fixture::shader, Fixture::matrix, Material::kDisplayWidth}) {
        f.denied = address;
        auto out = Seed();
        CHECK(f.Capture(Intent::kPlacedTintedAddress, Fixture::matrix, 0, out) ==
              Intent::Result::InvalidMemory);
        Unchanged(out);
    }
    f.denied = 0;
    for (uint32_t pointer : {0u, Fixture::matrix + 4, 0x01fffff0u, 0x70000000u}) {
        auto out = Seed();
        CHECK(f.Capture(Intent::kPlacedAddress, pointer, 0, out) == Intent::Result::InvalidMemory);
        Unchanged(out);
    }
    auto out = Seed();
    CHECK(Intent::Capture([](uint32_t, void*, size_t) -> bool {
        throw std::runtime_error("unavailable");
    }, Intent::kPlainAddress, Fixture::object, 0, 0, f.environment, out) ==
          Intent::Result::InvalidMemory);
    Unchanged(out);
}

void TestAliasesAndUnsupportedStates() {
    Fixture f;
    for (uint32_t segment : {0u, 0x80000000u, 0xa0000000u}) {
        f.Put(Fixture::object + 0x14, Fixture::positions | segment);
        f.Put(Fixture::object + 0x18, Fixture::colors | segment);
        f.Put(Material::kRenderTarget, Fixture::target | segment);
        Intent::Output out;
        f.reads.clear();
        CHECK(f.Capture(Intent::kPlacedTintedAddress, Fixture::matrix | segment,
                        0x40606060, out, Fixture::object | segment) == Intent::Result::Ok);
        CHECK(out.draw.verts[0].x == 5 && Color(out.draw.verts[0]) == 0x40606060u);
        for (uint32_t address : f.reads) CHECK(address < 0x02000000u);
    }
    auto out = Seed();
    f.Put(Fixture::shader + 0x64, 1);
    CHECK(f.Capture(Intent::kPlainAddress, 0, 0, out) == Intent::Result::UnsupportedMaterial);
    Unchanged(out);
    f.Put(Fixture::shader + 0x64, 14);
    f.Put(Fixture::target + 0xc, 0);
    CHECK(f.Capture(Intent::kPlainAddress, 0, 0, out) == Intent::Result::UnsupportedTarget);
    Unchanged(out);
    f.Put(Fixture::target + 0xc, 512);
    f.Put(Fixture::matrix, 0x7fc00000);
    CHECK(f.Capture(Intent::kPlacedAddress, Fixture::matrix, 0, out) ==
          Intent::Result::InvalidGeometry);
    Unchanged(out);
    f.Float(Fixture::matrix, 1);
    f.Put(Fixture::positions, 0x7f800000);
    CHECK(f.Capture(Intent::kPlainAddress, 0, 0, out) == Intent::Result::InvalidGeometry);
    Unchanged(out);
}
}  // namespace

int main() {
    TestAllEntriesAndOwnedState();
    TestRangesEntriesAndAtomicDecline();
    TestAliasesAndUnsupportedStates();
    std::printf("StripIntent: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
