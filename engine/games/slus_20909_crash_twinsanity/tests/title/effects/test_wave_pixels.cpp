#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <SDL.h>

#include <cstdio>
#include <fstream>
#include <iterator>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_device.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/screen_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/wave_oracle.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
void Compare(const std::vector<uint8_t>& original, const std::vector<uint8_t>& replacement) {
    CHECK(original.size() == replacement.size());
    if (original.size() != replacement.size()) return;
    size_t differences = 0, first = 0;
    for (size_t i = 0; i < original.size(); ++i) {
        if (original[i] == replacement[i]) continue;
        if (!differences) first = i;
        ++differences;
    }
    if (differences)
        std::fprintf(stderr, "wave pixel bytes differ: %zu; first %zu NEW %u replacement %u\n",
                     differences, first, original[first], replacement[first]);
    CHECK(differences == 0);
}
void Run(EffectsPixelDevice& backend, const WaveOracle& oracle, uint8_t scale) {
    constexpr uint32_t height = 64;
    const auto source = ScreenOracle::Wave(512, height);
    CHECK(source.size() == 2);
    if (source.size() != 2) return;
    EffectsPixelFixture model(backend, scale, height), replacement(backend, scale, height);
    title::WaveGeometry::Grid grid;
    CHECK(title::WaveGeometry::MakeGrid(512, height, grid) == title::WaveGeometry::Result::Ok);
    title::WaveGeometry::Waves waves;
    for (size_t i = 0; i < waves.rows.size(); ++i)
        waves.rows[i] = {float(i) / 7 - 1, float(15 - i) / 7 - 1, 0};
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        waves.amplitude = scenario == 0 ? 0.f : scenario == 1 ? 3.5f : -2.f;
        std::vector<uint8_t> pixels(size_t(512 * scale) * height * scale * 4);
        for (uint32_t y = 0; y < height * scale; ++y)
            for (uint32_t x = 0; x < 512 * scale; ++x) {
                const auto at = (size_t(y) * 512 * scale + x) * 4;
                pixels[at] = uint8_t(x + scenario * 13);
                pixels[at + 1] = uint8_t(y * 5 + scenario);
                pixels[at + 2] = uint8_t((x / 13 ^ y / 7) * 31);
                pixels[at + 3] = scenario == 4 ? 0 : uint8_t(32 + x % 160);
            }
        const float depth = scenario == 3 ? 1.f : 0.f;
        model.Initial(source[0], pixels, depth);
        replacement.Initial(source[0], pixels, depth);
        backend.calls.clear(); backend.record = true;
        CHECK(model.pipeline.DrawGeneric(source[0]).drew);
        const auto geometry = oracle.Run(grid, waves);
        for (const auto& strip : geometry) {
            auto draw = source[1];
            draw.verts = strip.verts;
            draw.vertex_count = strip.vertex_count;
            draw.snapshot = strip.snapshot;
            CHECK(model.pipeline.DrawGeneric(draw).drew);
        }
        auto original_calls = std::move(backend.calls); backend.calls.clear();
        CHECK(replacement.effects.Wave(replacement.frame, grid, waves) == title::EffectsScene::Result::Ok);
        backend.record = false;
        CHECK(original_calls.size() == 65 && backend.calls.size() == original_calls.size());
        Compare(EffectsPixelDevice::CopyRgb(original_calls[0], 512, height),
                EffectsPixelDevice::CopyRgb(backend.calls[0], 512, height));
        for (size_t i = 0; i < original_calls.size() && i < backend.calls.size(); ++i) {
            const auto& a = original_calls[i].draw.verts; const auto& b = backend.calls[i].draw.verts;
            CHECK(a.size() == b.size());
            CHECK(ps2r::gs::GsdlFormat::VertexHash(a.data(), a.size()) ==
                  ps2r::gs::GsdlFormat::VertexHash(b.data(), b.size()));
        }
        const auto expected = model.Pixels(), actual = replacement.Pixels();
        if (expected != actual) std::fprintf(stderr, "wave scale %u scenario %u\n", scale, scenario);
        Compare(expected, actual);
        CHECK(glGetError() == GL_NO_ERROR);
        for (size_t i = 3; i < actual.size(); i += 4) CHECK(actual[i] == pixels[i]);
        if (scenario == 3) CHECK(actual == pixels); // depth rejects the whole composite
        if (scenario == 1 || scenario == 2) CHECK(actual != pixels); // actual deformation must be visible
    }
    CHECK(replacement.effects.Counts().wave_passes == 5);
    CHECK(replacement.effects.WaveCounts().allocations == 2);
    CHECK(replacement.effects.WaveCounts().strips == 5 * 64);
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::puts("SKIP: static NTSC wave program required"); return 77; }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    WaveOracle oracle({std::istreambuf_iterator<char>(input), {}});
    ps2r::platform::Window window;
    ps2r::platform::GlContext context;
    if (!window.Create(true, 128, 64, "wave interface self-oracle") || !context.Create(window)) return 77;
    EffectsPixelDevice device;
    CHECK(device.Init([&] { window.Swap(); }));
    Run(device, oracle, 1);
    Run(device, oracle, 2);
    std::printf("WavePixels: %d checks, %d failures (NEW producers/program/pipeline, same input)\n",
                checks, failures);
    return failures ? 1 : 0;
}
