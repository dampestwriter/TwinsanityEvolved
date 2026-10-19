#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {
using Material = title::MaterialState;
using Result = Material::Result;
using Draw = ps2r::gs::DrawIr;
int checks = 0, failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

struct TestMaterialState {
    static constexpr uint32_t shader = 0x4000, target = 0x6000;
    // OLEG's flat settings after a zero-filled allocation: mix blending,
    // GEQUAL, Gouraud, linear filtering, no depth writes. Other tests seed the
    // constructor's untouched bits independently.
    static constexpr uint64_t flat = 0x0880000004400001ull;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x310000, 0xa5);
    std::vector<std::pair<uint32_t, size_t>> reads;
    uint32_t deny = 0;
    Material::Environment environment{true, false, 0, 0, 0, 0, 0, 0x6071243571603524ull};
    Material::Frame frame;

    void Put(uint32_t address, uint64_t value, size_t count = 4) {
        for (size_t i = 0; i < count; ++i) bytes.at(address + i) = uint8_t(value >> (8 * i));
    }
    Material::Read Reader() {
        return [this](uint32_t address, void* output, size_t count) {
            reads.emplace_back(address, count);
            if (uint64_t(address) + count > bytes.size() ||
                (deny && address <= deny && uint64_t(address) + count > deny)) return false;
            std::memcpy(output, bytes.data() + address, count);
            return true;
        };
    }
    TestMaterialState() {
        Put(Material::kFlatMaterial, shader);
        Put(Material::kFlatMaterial + 0x10, 1);
        Put(Material::kFlatMaterial + 0x18, 2, 8);
        Put(Material::kFlatMaterial + 0x20, 24);
        Put(shader, flat, 8);
        Put(shader + 0x64, 14);
        Put(shader + 0x6c, Material::kFlatShaderVtable);
        Put(target + 4, 0); Put(target + 8, 0);
        Put(target + 0xc, 512); Put(target + 0x10, 448);
        Put(target + 0x18, 512); Put(target + 0x1c, 448);
        Put(Material::kDisplayWidth, 512);
        Put(Material::kDisplayHeight, 448);
        Put(Material::kDisplayPixelBytes, 2);
        Put(Material::kDepthBufferPage, 168);
        CHECK(Material::ReadFrame(Reader(), environment, frame) == Result::Ok);
        reads.clear();
    }
    Result Build(Material::Output& out, uint32_t material = Material::kFlatMaterial,
                 uint32_t rectangle = target) {
        return Material::Build(Reader(), material, rectangle, frame, out);
    }
};

void Same(const Material::Output& actual, const Material::Output& expected) {
    const auto a = actual.draw.ToDrawRec(), b = expected.draw.ToDrawRec();
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(actual.bucket == expected.bucket);
    CHECK(actual.coordinates.display_width == expected.coordinates.display_width);
    CHECK(actual.coordinates.display_height == expected.coordinates.display_height);
    CHECK(actual.coordinates.offset_x == expected.coordinates.offset_x);
    CHECK(actual.coordinates.offset_y == expected.coordinates.offset_y);
    CHECK(actual.draw.raster.dthe == expected.draw.raster.dthe);
    CHECK(actual.draw.raster.dimx == expected.draw.raster.dimx);
    CHECK(actual.draw.tex.mxl == expected.draw.tex.mxl);
    CHECK(actual.draw.tex.lcm == expected.draw.tex.lcm);
    CHECK(actual.draw.tex.l == expected.draw.tex.l);
    CHECK(actual.draw.tex.k == expected.draw.tex.k);
    for (size_t i = 0; i < 6; ++i) {
        CHECK(actual.draw.tex.mip_tbp[i] == expected.draw.tex.mip_tbp[i]);
        CHECK(actual.draw.tex.mip_tbw[i] == expected.draw.tex.mip_tbw[i]);
    }
    CHECK(actual.draw.verts.size() == expected.draw.verts.size());
    if (!actual.draw.verts.empty() && actual.draw.verts.size() == expected.draw.verts.size())
        CHECK(std::memcmp(actual.draw.verts.data(), expected.draw.verts.data(),
                          actual.draw.verts.size() * sizeof(ps2r::gs::VertexRec)) == 0);
}

Material::Output Sentinel() {
    Material::Output out;
    out.bucket = 19;
    out.coordinates = {13, 17, 23, 29};
    out.draw.seq = 31;
    out.draw.src_frame = 73;
    out.draw.identity = {1, 2, 3, 4, 5};
    out.draw.tex_enabled = true;
    out.draw.tex.mxl = 3; out.draw.tex.k = -173;
    out.draw.tex.mip_tbp[5] = 0x170000;
    out.draw.raster.dthe = 1;
    out.draw.raster.dimx = 0x1718192021222324ull;
    out.draw.raster.frame = {0x80000, 640, 2, 0x80000000};
    out.draw.raster.test = {true, 7, 64, 3, true, true, true, 3};
    out.draw.verts.resize(1);
    out.draw.verts[0].x = 35;
    out.draw.verts[0].r = 255;
    out.draw.vertex_count = 1;
    out.draw.snapshot.vertex_hash = 0xbad;
    return out;
}

void Declines(TestMaterialState& fixture, Result result,
              uint32_t material = Material::kFlatMaterial,
              uint32_t target = TestMaterialState::target) {
    auto out = Sentinel();
    const auto expected = out;
    const auto memory = fixture.bytes;
    CHECK(fixture.Build(out, material, target) == result);
    Same(out, expected);
    CHECK(fixture.bytes == memory);
}

void TestRetailFlatState() {
    TestMaterialState f;
    CHECK(f.frame.display_width == 512 && f.frame.display_height == 448);
    CHECK(f.frame.display_pixel_bytes == 2 && f.frame.depth_buffer_page == 168);
    const auto memory = f.bytes;
    auto out = Sentinel();
    CHECK(f.Build(out) == Result::Ok);
    // This expectation expresses the source material and target in DrawIr units,
    // without reading/replaying the legacy shader register allocation.
    Material::Output expected;
    expected.bucket = 24;
    expected.coordinates = {512, 448, 0, 0};
    Draw& draw = expected.draw;
    draw.prim_type = 4; draw.gouraud = true; draw.use_fst = true;
    draw.alpha_blend = true; draw.tex.fst = true;
    draw.off_x = 1792; draw.off_y = 1824;
    draw.raster.frame = {0x70000, 512, 0, 0};
    draw.raster.zbuf = {0x150000, 0, true};
    draw.raster.test = {false, 0, 0, 0, false, false, true, 2};
    draw.raster.alpha = {0, 1, 0, 1, 0};
    draw.raster.scissor = {0, 0, 511, 447};
    draw.raster.colclamp = true; draw.raster.fba = true;
    draw.raster.dimx = 0x6071243571603524ull;
    Same(out, expected);
    CHECK(!out.draw.tex_enabled && !out.draw.fog);
    CHECK(out.draw.verts.empty() && out.draw.vertex_count == 0);
    CHECK(f.bytes == memory);
    CHECK(f.reads.size() == 3);
    CHECK(f.reads[0] == std::make_pair(Material::kFlatMaterial, size_t(0x24)));
    CHECK(f.reads[1] == std::make_pair(TestMaterialState::shader, size_t(0x70)));
    CHECK(f.reads[2] == std::make_pair(TestMaterialState::target, size_t(0x20)));
}

void TestUnusedFieldsAndEnvironment() {
    TestMaterialState f;
    auto base = Material::Output{};
    CHECK(f.Build(base) == Result::Ok);
    // ShaderConstruct does not clear ATST/AREF/AFAIL/DATM or STQ; the disabled
    // tests still carry their source values, and PRMODE.FST is !STQ.
    const uint64_t residue = (uint64_t{7} << 6) | (uint64_t{129} << 9) |
        (uint64_t{2} << 17) | (uint64_t{1} << 20) | (uint64_t{1} << 28);
    f.Put(TestMaterialState::shader, TestMaterialState::flat | residue, 8);
    Material::Output out;
    CHECK(f.Build(out) == Result::Ok);
    CHECK(!out.draw.use_fst && !out.draw.tex.fst);
    CHECK(!out.draw.raster.test.ate && !out.draw.raster.test.date);
    CHECK(out.draw.raster.test.atst == 7 && out.draw.raster.test.aref == 129);
    CHECK(out.draw.raster.test.afail == 2 && out.draw.raster.test.datm);
    // Preset blending ignores the dormant custom formula; untextured drawing
    // ignores filtering, scrolls, animation and colour/texture object fields.
    for (unsigned bit : {23, 24, 25, 31, 32, 37, 39, 40, 47, 54, 55, 57, 58, 60, 61, 62, 63}) {
        f.Put(TestMaterialState::shader, TestMaterialState::flat ^ (uint64_t{1} << bit), 8);
        CHECK(f.Build(out) == Result::Ok);
        Same(out, base);
    }
    f.Put(TestMaterialState::shader, TestMaterialState::flat, 8);
    f.frame.environment = {false, true, 2, 17, 29, 43, 1, 0x1234567012345670ull};
    CHECK(f.Build(out) == Result::Ok);
    CHECK(!out.draw.raster.colclamp && out.draw.raster.pabe);
    CHECK(out.draw.scanmsk == 2 && out.draw.raster.dthe == 1);
    CHECK(out.draw.raster.fogcol_r == 17 && out.draw.raster.fogcol_g == 29 &&
          out.draw.raster.fogcol_b == 43);
    CHECK(out.draw.raster.dimx == 0x1234567012345670ull);
}

void TestTargetAndFrameHaveDistinctDimensions() {
    TestMaterialState f;
    f.Put(TestMaterialState::target + 4, 7);
    f.Put(TestMaterialState::target + 8, 11);
    f.Put(TestMaterialState::target + 0xc, 400);
    f.Put(TestMaterialState::target + 0x10, 240);
    f.Put(TestMaterialState::target + 0x18, 511);
    f.Put(TestMaterialState::target + 0x1c, 479);
    Material::Output out;
    CHECK(f.Build(out) == Result::Ok);
    CHECK(out.coordinates.display_width == 511 && out.coordinates.display_height == 479);
    CHECK(out.coordinates.offset_x == 7 && out.coordinates.offset_y == 11);
    CHECK(out.draw.off_x == 1792 && out.draw.off_y == 1824);
    CHECK(out.draw.raster.scissor.x0 == 7 && out.draw.raster.scissor.x1 == 406);
    CHECK(out.draw.raster.scissor.y0 == 11 && out.draw.raster.scissor.y1 == 250);
    // The shared target truncates global width to FBW's 64-pixel units and
    // places the drawn buffer at the preceding complete display-buffer page.
    f.frame.display_width = 513; f.frame.display_height = 449;
    f.frame.depth_buffer_page = 169;
    CHECK(f.Build(out) == Result::Ok);
    CHECK(out.draw.off_x == 1792 && out.draw.off_y == 1824);
    CHECK(out.draw.raster.frame.fbw == 512 && out.draw.raster.frame.fbp == 0x70000);
    CHECK(out.draw.raster.zbuf.zbp == 0x152000);
    CHECK(out.coordinates.display_width == 511 && out.coordinates.display_height == 479);
    // Maximum scissor coordinate and single-pixel rectangles remain valid.
    f.Put(TestMaterialState::target + 4, 2047);
    f.Put(TestMaterialState::target + 8, 2047);
    f.Put(TestMaterialState::target + 0xc, 1);
    f.Put(TestMaterialState::target + 0x10, 1);
    CHECK(f.Build(out) == Result::Ok);
    CHECK(out.draw.raster.scissor.x0 == 2047 && out.draw.raster.scissor.x1 == 2047);
}

void TestUnsupportedMaterialsAndShaders() {
    TestMaterialState f;
    for (uint32_t material : {0u, 0x5000u, 0xfffffff0u}) {
        f.reads.clear();
        Declines(f, Result::UnsupportedMaterial, material);
        CHECK(f.reads.empty());
    }
    for (uint32_t count : {0u, 2u, 4u, 0xffffffffu}) {
        f.Put(Material::kFlatMaterial + 0x10, count);
        Declines(f, Result::UnsupportedMaterial);
    }
    f.Put(Material::kFlatMaterial + 0x10, 1);
    f.Put(Material::kFlatMaterial + 0x18, 0x100000002ull, 8);
    Declines(f, Result::UnsupportedMaterial);
    f.Put(Material::kFlatMaterial + 0x18, 2, 8);
    f.Put(Material::kFlatMaterial + 0x20, 27);
    Declines(f, Result::UnsupportedMaterial);
    f.Put(Material::kFlatMaterial + 0x20, 24);
    f.Put(TestMaterialState::shader + 0x64, 1);
    Declines(f, Result::UnsupportedShader);
    f.Put(TestMaterialState::shader + 0x64, 14);
    f.Put(TestMaterialState::shader + 0x6c, Material::kFlatShaderVtable + 8);
    Declines(f, Result::UnsupportedShader);
    f.Put(TestMaterialState::shader + 0x6c, Material::kFlatShaderVtable);
    for (unsigned bit : {0, 1, 5, 19, 21, 22, 26, 27, 29, 30, 38, 56, 59}) {
        f.Put(TestMaterialState::shader, TestMaterialState::flat ^ (uint64_t{1} << bit), 8);
        Declines(f, Result::UnsupportedShader);
    }
}

void TestInvalidReadsAndRanges() {
    TestMaterialState f;
    for (uint32_t deny : {Material::kFlatMaterial, TestMaterialState::shader,
                          TestMaterialState::target}) {
        f.deny = deny;
        Declines(f, Result::InvalidMemory);
    }
    f.deny = 0;
    for (uint32_t pointer : {0u, 0xfffffff0u, 0x70000000u}) {
        f.Put(Material::kFlatMaterial, pointer);
        Declines(f, Result::InvalidMemory);
    }
    f.Put(Material::kFlatMaterial, TestMaterialState::shader);
    for (uint32_t pointer : {0u, 0xfffffff0u, 0x70000000u})
        Declines(f, Result::InvalidMemory, Material::kFlatMaterial, pointer);
    auto out = Sentinel();
    auto expected = out;
    CHECK(Material::Build({}, Material::kFlatMaterial, TestMaterialState::target, f.frame, out)
          == Result::InvalidMemory);
    Same(out, expected);
    const auto original = f.bytes;
    const std::array<std::pair<uint32_t, uint32_t>, 10> bad{{
        {4, 0xffffffff}, {8, 2048}, {0xc, 0}, {0x10, 0}, {0xc, 0xffffffff},
        {0x10, 2049}, {0x18, 0}, {0x1c, 0xffffffff}, {0x18, 4097}, {0x1c, 4097}}};
    for (const auto& value : bad) {
        f.bytes = original;
        f.Put(TestMaterialState::target + value.first, value.second);
        Declines(f, Result::UnsupportedTarget);
    }
    f.bytes = original;
    f.Put(TestMaterialState::target + 4, 2047);
    f.Put(TestMaterialState::target + 0xc, 2);
    Declines(f, Result::UnsupportedTarget);
}

void TestFramePreflightAndAtomicCapture() {
    TestMaterialState f;
    const auto valid = f.frame;
    for (uint32_t width : {0u, 63u, 4096u, 0xffffffffu}) {
        f.frame = valid; f.frame.display_width = width;
        f.reads.clear();
        Declines(f, Result::UnsupportedFrame);
        CHECK(f.reads.empty());
    }
    f.frame = valid; f.frame.display_height = 0;
    Declines(f, Result::UnsupportedFrame);
    f.frame = valid; f.frame.display_width = 2048; f.frame.display_height = 2048;
    Declines(f, Result::UnsupportedFrame);  // allocation exceeds four megabytes
    f.frame = valid; f.frame.display_pixel_bytes = 4;
    Declines(f, Result::UnsupportedFrame);
    f.frame = valid; f.frame.depth_buffer_page = 512;
    Declines(f, Result::UnsupportedFrame);
    f.frame = valid; f.frame.environment.scan_mask = 4;
    Declines(f, Result::UnsupportedFrame);
    f.frame = valid; f.frame.environment.dither = 2;
    Declines(f, Result::UnsupportedFrame);
    for (uint32_t deny : {Material::kDisplayWidth, Material::kDisplayHeight,
                          Material::kDisplayPixelBytes, Material::kDepthBufferPage}) {
        f.deny = deny;
        Material::Frame out{64, 17, 2, 77, {}};
        CHECK(Material::ReadFrame(f.Reader(), f.environment, out) == Result::InvalidMemory);
        CHECK(out.display_width == 64 && out.display_height == 17);
        CHECK(out.display_pixel_bytes == 2 && out.depth_buffer_page == 77);
        CHECK(!out.environment.colour_clamp && out.environment.dither_matrix == 0);
    }
    f.deny = 0;
    f.Put(Material::kDisplayWidth, 0xffffffff);
    Material::Frame out = valid;
    CHECK(Material::ReadFrame(f.Reader(), {}, out) == Result::UnsupportedFrame);
    CHECK(out.display_width == valid.display_width && out.display_height == valid.display_height);
    CHECK(out.environment.dither_matrix == valid.environment.dither_matrix);
}
}  // namespace

int main() {
    TestRetailFlatState();
    TestUnusedFieldsAndEnvironment();
    TestTargetAndFrameHaveDistinctDimensions();
    TestUnsupportedMaterialsAndShaders();
    TestInvalidReadsAndRanges();
    TestFramePreflightAndAtomicCapture();
    std::printf("MaterialState: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
