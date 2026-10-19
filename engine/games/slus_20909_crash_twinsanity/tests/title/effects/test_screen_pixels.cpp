#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/gs/format/psm_layout.h"
#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_renderer.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_device.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/screen_oracle.h"

// The colour filter's pixel self-oracle. Three arms on the SAME input pixels
// and depth: (1) the owned pass (EffectsScene::Screen -> ScreenRenderer, the
// backend's depth-byte palette serve), (2) the closed form of what the retail
// packets ask the GS for -- index = Z[15:8], GREATER at 0x10000, Cv = ((Cd -
// Cs) * As >> 7) + Cs with Cs = the palette entry -- and (3) the model: the
// complete generated CopyDepthBuffer + FilterColours bodies' packets through
// the current NEW DrawPipeline with the palette uploaded into its VRAM image.
// (1) must match (2) to the GL blend's rounding; (3) is attributed, never a
// gate: it reports whether the model path changes any pixel on this fixture.
namespace {
int checks = 0, failures = 0;
bool debug_dump = false;   // --debug: print the index texels / first pixels per scenario (no getenv; CoreSingleGetenvSite)
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
constexpr uint32_t kHeight = 64, kWidth = 512;

struct Input {
    std::vector<uint8_t> rgba;   // physical (scale applied)
    std::vector<uint32_t> z;     // logical, the GS Z of each pixel
    std::vector<float> depth;    // physical, z / 2^32
};
Input MakeInput(uint8_t scale, unsigned scenario) {
    Input input;
    input.rgba.resize(size_t(kWidth * scale) * kHeight * scale * 4);
    input.z.resize(size_t(kWidth) * kHeight);
    input.depth.resize(size_t(kWidth * scale) * kHeight * scale);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            // Every index byte, both sides of the GREATER boundary, the far
            // clear (0) and a few near pixels.
            uint32_t z = ((y * kWidth + x) * 37u) & 0x1ffffu;
            if (scenario == 1) z = 0;
            if (scenario == 2) z = 0x10000 + x;
            if (scenario == 3) z = (x & 1) ? 0xffff : 0x10000;
            input.z[y * kWidth + x] = z;
        }
    for (uint32_t py = 0; py < kHeight * scale; ++py)
        for (uint32_t px = 0; px < kWidth * scale; ++px) {
            const auto at = size_t(py) * kWidth * scale + px;
            input.rgba[at * 4] = uint8_t(px / scale + scenario * 13);
            input.rgba[at * 4 + 1] = uint8_t((py / scale) * 5 + scenario);
            input.rgba[at * 4 + 2] = uint8_t(((px / scale) / 13 ^ (py / scale) / 7) * 31);
            input.rgba[at * 4 + 3] = uint8_t(32 + (px / scale) % 160);
            input.depth[at] = float(input.z[(py / scale) * kWidth + px / scale]) / 4294967296.0f;
        }
    return input;
}
title::ScreenPalettes::Palette MakePalette(unsigned scenario) {
    // Storage (CSM1) order as the authored tables are; graded alpha like the
    // retail fog palettes (0x5a..0x7e), one entry at/above 128 to pin the cap.
    title::ScreenPalettes::Palette storage{};
    for (uint32_t logical = 0; logical < 256; ++logical) {
        const uint32_t r = scenario == 2 ? 0 : logical, g = (255 - logical), b = scenario == 1 ? 0xf7 : (logical * 3) & 0xff;
        uint32_t a = 0x5a + (logical * 0x24) / 255;
        if (logical == 200) a = 0x80;
        if (logical == 201) a = 0xff;
        if (logical == 7) a = 0;
        storage[title::ScreenPalettes::Slot(logical)] = (a << 24) | (b << 16) | (g << 8) | r;
    }
    return storage;
}
void Depth(EffectsPixelFixture& fixture, const Input& input, uint8_t scale) {
    auto* target = fixture.cache.Find(fixture.frame.color_buffer);
    const ps2r::render::Rect rectangle{0, 0, int(kWidth * scale), int(kHeight * scale)};
    fixture.surfaces.Upload(target->views.depth, rectangle, input.depth.data(), size_t(kWidth * scale) * sizeof(float));
    std::vector<float> actual(input.depth.size());
    fixture.surfaces.Readback(target->views.depth, rectangle, actual.data(), size_t(kWidth * scale) * sizeof(float));
    CHECK(actual == input.depth);
}
// The closed form of the retail request on the same inputs (RGB; the alpha
// byte takes the palette alpha under FBMSK 0x80000000 and is not compared).
// `shift` reads the index from the diagonal neighbour (the host's NEAREST
// floor of the half-texel UVs); `mask` keeps only those index bits.
std::vector<uint8_t> Reference(const Input& input, const title::ScreenPalettes::Palette& storage, uint8_t scale,
                               uint32_t shift = 0, uint32_t mask = 0xff) {
    title::ScreenPalettes::Palette logical;
    title::ScreenPalettes::Logical(storage, logical);
    auto out = input.rgba;
    for (uint32_t py = 0; py < kHeight * scale; ++py)
        for (uint32_t px = 0; px < kWidth * scale; ++px) {
            const uint32_t z = input.z[(py / scale) * kWidth + px / scale];
            if (!(0x10000 > z)) continue;
            const uint32_t ix = std::min(px / scale + shift, kWidth - 1), iy = std::min(py / scale + shift, kHeight - 1);
            const uint32_t zi = input.z[iy * kWidth + ix];
            const uint32_t colour = logical[(zi >> 8) & mask];
            const uint32_t as = std::min<uint32_t>(colour >> 24, 128);
            uint8_t* pixel = &out[(size_t(py) * kWidth * scale + px) * 4];
            for (unsigned channel = 0; channel < 3; ++channel) {
                const int cs = int((colour >> (channel * 8)) & 0xff), cd = pixel[channel];
                int value = (((cd - cs) * int(as)) >> 7) + cs;
                pixel[channel] = uint8_t(std::min(std::max(value, 0), 255));
            }
        }
    return out;
}
struct Difference { size_t pixels = 0; int max = 0; size_t first = 0; };
Difference CompareRgb(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Difference d;
    for (size_t i = 0; i < a.size() && i < b.size(); i += 4) {
        int worst = 0;
        for (unsigned c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(a[i + c]) - int(b[i + c])));
        if (!worst) continue;
        if (!d.pixels) d.first = i / 4;
        ++d.pixels;
        d.max = std::max(d.max, worst);
    }
    return d;
}
void Owned(EffectsPixelDevice& backend, uint8_t scale) {
    const auto filter = ScreenOracle::Filter(kHeight, false);
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        const auto input = MakeInput(scale, scenario);
        const auto storage = MakePalette(scenario);
        EffectsPixelFixture replacement(backend, scale, kHeight);
        replacement.Initial(filter, input.rgba, 0.f);
        Depth(replacement, input, scale);
        title::EffectsScene::ScreenFrame work;
        work.screen.height = kHeight;
        work.screen.filter = true;
        work.screen.palette = storage;
        backend.calls.clear(); backend.record = true;
        CHECK(replacement.effects.Screen(replacement.frame, work) == title::EffectsScene::Result::Ok);
        backend.record = false;
        CHECK(backend.calls.size() == 2);
        if (backend.calls.size() == 2) {
            const auto& lookup = backend.calls[0];
            CHECK(lookup.texture.transform.idx_fmt == 7 && lookup.texture.transform.idx_self == 8);
            CHECK(lookup.parameters.clut.id != 0 && lookup.texture.owner.clut.id == lookup.parameters.clut.id);
            CHECK(lookup.parameters.depth.id == 0 && !lookup.draw.alpha_blend && lookup.draw.verts.size() == 96);
            const auto& call = backend.calls[1];
            CHECK(call.texture.transform.idx_fmt == 0 && call.texture.owner.surface.id == lookup.parameters.color.id);
            CHECK(call.draw.raster.test.ztst == 3 && call.draw.raster.zbuf.zmsk && call.draw.alpha_blend);
            CHECK(call.draw.raster.frame.fbmsk == 0x80000000u && call.draw.verts.size() == 96);
            CHECK(call.draw.tex.psm == 0x1b && call.draw.tex.tfx == 1 && call.parameters.depth.id != 0);
        }
        const auto actual = replacement.Pixels();
        const auto expected = Reference(input, storage, scale);
        const auto difference = CompareRgb(expected, actual);
        if (debug_dump && backend.calls.size() == 2) {
            std::vector<uint8_t> index(size_t(kWidth * scale) * kHeight * scale * 4);
            backend.Readback(backend.calls[0].parameters.color, {0, 0, int(kWidth * scale), int(kHeight * scale)},
                             index.data(), size_t(kWidth * scale) * 4);
            title::ScreenPalettes::Palette logical; title::ScreenPalettes::Logical(storage, logical);
            size_t wrong = 0, first_wrong = 0;
            for (uint32_t py = 0; py < kHeight * scale; ++py)
                for (uint32_t px = 0; px < kWidth * scale; ++px) {
                    const uint32_t z = input.z[(py / scale) * kWidth + px / scale];
                    const uint32_t c = logical[(z >> 8) & 0xff];
                    const auto* v = &index[(size_t(py) * kWidth * scale + px) * 4];
                    if (v[0] != (c & 0xff) || v[1] != ((c >> 8) & 0xff) || v[2] != ((c >> 16) & 0xff)) {
                        if (!wrong) first_wrong = size_t(py) * kWidth * scale + px;
                        ++wrong;
                    }
                }
            std::fprintf(stderr, "scen %u scale %u: P wrong texels %zu first %zu\n", scenario, scale, wrong, first_wrong);
            for (uint32_t x = 0; x < 6; ++x) {
                const uint32_t z = input.z[x];
                const auto* px = &index[x * 4];
                std::fprintf(stderr, "scen %u x=%u z=%05x idx=%02x P=%02x %02x %02x %02x clut=%08x in=%02x %02x %02x out=%02x %02x %02x exp=%02x %02x %02x\n",
                    scenario, x, z, (z >> 8) & 0xff, px[0], px[1], px[2], px[3], logical[(z >> 8) & 0xff],
                    input.rgba[x*4], input.rgba[x*4+1], input.rgba[x*4+2], actual[x*4], actual[x*4+1], actual[x*4+2],
                    expected[x*4], expected[x*4+1], expected[x*4+2]);
            }
        }
        if (difference.pixels)
            std::fprintf(stderr, "filter scale %u scenario %u: %zu pixels differ from the closed form, max %d, first %zu\n",
                         scale, scenario, difference.pixels, difference.max, difference.first);
        CHECK(difference.max <= 3); // the GL blend's alpha scaling (As/128 as a/255 of 2*As) and float rounding
        CHECK(glGetError() == GL_NO_ERROR);
        // Pixels the GREATER test rejects are untouched in every byte; admitted
        // pixels change unless the entry is the identity (alpha 128).
        size_t rejected = 0, changed = 0, admitted = 0;
        for (uint32_t py = 0; py < kHeight * scale; ++py)
            for (uint32_t px = 0; px < kWidth * scale; ++px) {
                const auto at = (size_t(py) * kWidth * scale + px) * 4;
                const uint32_t z = input.z[(py / scale) * kWidth + px / scale];
                if (z >= 0x10000) {
                    ++rejected;
                    CHECK(std::memcmp(&actual[at], &input.rgba[at], 4) == 0);
                } else {
                    ++admitted;
                    if (std::memcmp(&actual[at], &input.rgba[at], 3) != 0) ++changed;
                }
            }
        if (scenario == 2) CHECK(rejected == actual.size() / 4 && changed == 0);
        else CHECK(admitted > 0 && changed > admitted / 2);
        if (scenario == 3) CHECK(rejected == actual.size() / 8 && admitted == actual.size() / 8);
        CHECK(replacement.effects.Counts().screen_passes == 1);
        CHECK(replacement.effects.ScreenCounts().filter_draws == 1 && replacement.effects.ScreenCounts().palette_uploads == 1);
        // The same palette again uploads nothing; a different one re-uploads.
        work.screen.palette = storage;
        CHECK(replacement.effects.Screen(replacement.frame, work) == title::EffectsScene::Result::Ok);
        CHECK(replacement.effects.ScreenCounts().palette_uploads == 1);
        work.screen.palette[3] ^= 0x00ffffffu;
        CHECK(replacement.effects.Screen(replacement.frame, work) == title::EffectsScene::Result::Ok);
        CHECK(replacement.effects.ScreenCounts().palette_uploads == 2);
    }
}
// The blur over the same fixture: the far pixels (GREATER at 0x7fff) take the
// shrunk-and-stretched copy; near pixels are untouched.
void Blur(EffectsPixelDevice& backend, uint8_t scale) {
    const auto filter = ScreenOracle::Filter(kHeight, false);
    auto input = MakeInput(scale, 0);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) input.z[y * kWidth + x] = x < 256 ? 0x100 : 0x8000;
    for (uint32_t py = 0; py < kHeight * scale; ++py)
        for (uint32_t px = 0; px < kWidth * scale; ++px)
            input.depth[size_t(py) * kWidth * scale + px] = float(input.z[(py / scale) * kWidth + px / scale]) / 4294967296.0f;
    EffectsPixelFixture replacement(backend, scale, kHeight);
    replacement.Initial(filter, input.rgba, 0.f);
    Depth(replacement, input, scale);
    title::EffectsScene::ScreenFrame work;
    work.screen.height = kHeight;
    work.screen.blur = true;
    backend.calls.clear(); backend.record = true;
    CHECK(replacement.effects.Screen(replacement.frame, work) == title::EffectsScene::Result::Ok);
    backend.record = false;
    CHECK(backend.calls.size() == 3);
    const auto actual = replacement.Pixels();
    size_t changed_far = 0, changed_near = 0;
    for (uint32_t py = 0; py < kHeight * scale; ++py)
        for (uint32_t px = 0; px < kWidth * scale; ++px) {
            const auto at = (size_t(py) * kWidth * scale + px) * 4;
            const bool changed = std::memcmp(&actual[at], &input.rgba[at], 3) != 0;
            if (px / scale < 256) changed_far += changed; else changed_near += changed;
        }
    CHECK(changed_near == 0 && changed_far > size_t(kWidth / 2) * kHeight * scale * scale / 4);
    CHECK(replacement.effects.ScreenCounts().blur_passes == 1 && replacement.effects.ScreenCounts().allocations == 2);
    CHECK(glGetError() == GL_NO_ERROR);
}
// The model arm, attributed: the generated CopyDepthBuffer + FilterColours
// packets through the current NEW DrawPipeline with the palette in its VRAM.
void Model(EffectsPixelDevice& backend, uint8_t scale) {
    const auto input = MakeInput(scale, 0);
    const auto storage = MakePalette(0);
    const auto copies = ScreenOracle::DepthCopy(kHeight);
    auto filter = ScreenOracle::Filter(kHeight, false);
    EffectsPixelFixture model(backend, scale, kHeight);
    model.Initial(filter, input.rgba, 0.f);
    Depth(model, input, scale);
    // The palette packet's image: 16x16 CT32 at the CLUT block, in storage order.
    for (uint32_t entry = 0; entry < 256; ++entry) {
        const uint32_t address = ps2r::gs::PsmLayout::PixelByteAddress(ps2r::gs::Psm::CT32, entry % 16, entry / 16,
                                                                       filter.tex.cbp, 1);
        model.memory.WriteStream(address, reinterpret_cast<const uint8_t*>(&storage[entry]), 4);
    }
    model.memory.SettleWrites();
    bool drew = true;
    for (const auto& copy : copies) drew = model.pipeline.DrawGeneric(copy).drew && drew;
    const bool filter_drew = model.pipeline.DrawGeneric(filter).drew;
    const auto actual = model.Pixels();
    const auto expected = Reference(input, storage, scale);
    const auto vs_reference = CompareRgb(expected, actual), vs_input = CompareRgb(input.rgba, actual);
    const auto vs_shifted = CompareRgb(Reference(input, storage, scale, 1), actual);
    const auto vs_two_bits = CompareRgb(Reference(input, storage, scale, 0, 0xc0), actual);
    const auto vs_shifted_two_bits = CompareRgb(Reference(input, storage, scale, 1, 0xc0), actual);
    size_t within = 0;
    for (size_t i = 0; i < actual.size(); i += 4) {
        int worst = 0;
        for (unsigned c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(actual[i + c]) - int(expected[i + c])));
        within += worst <= 3;
    }
    std::printf("ScreenPixels model arm (scale %u): depth copies drew=%d filter drew=%d; pixels changed from the input "
                "%zu of %zu; vs the closed form: %zu differ (max %d), %zu within 3; vs the diagonal-neighbour index: %zu "
                "differ (max %d); vs the two-bit index: %zu differ (max %d); vs both: %zu differ (max %d)\n",
                scale, drew, filter_drew, vs_input.pixels, actual.size() / 4, vs_reference.pixels, vs_reference.max,
                within, vs_shifted.pixels, vs_shifted.max, vs_two_bits.pixels, vs_two_bits.max,
                vs_shifted_two_bits.pixels, vs_shifted_two_bits.max);
    CHECK(glGetError() == GL_NO_ERROR);
}
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) debug_dump = debug_dump || std::string(argv[i]) == "--debug";
    ps2r::platform::Window window;
    ps2r::platform::GlContext context;
    if (!window.Create(true, 128, 64, "screen effects self-oracle") || !context.Create(window)) return 77;
    EffectsPixelDevice device;
    CHECK(device.Init([&] { window.Swap(); }));
    Owned(device, 1);
    Owned(device, 2);
    Blur(device, 1);
    Blur(device, 2);
    Model(device, 1);
    std::printf("ScreenPixels: %d checks, %d failures (owned filter/blur vs the closed form of the retail request; "
                "model arm attributed)\n", checks, failures);
    return failures ? 1 : 0;
}
