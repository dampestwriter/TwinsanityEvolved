#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <SDL.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_material_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_oracle.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_fixture.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_device.h"

namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) throw std::runtime_error( \
    std::string("distortion pixels line ") + std::to_string(__LINE__) + ": " + #value); } while (0)
void Compare(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual) {
    CHECK(expected.size() == actual.size());
    size_t differences = 0, first = 0;
    for (size_t i = 0; i < expected.size(); ++i) if (expected[i] != actual[i]) {
        if (!differences) first = i;
        ++differences;
    }
    if (differences)
        std::fprintf(stderr, "%zu distortion byte differences; first %zu NEW %u owned %u\n",
                     differences, first, expected[first], actual[first]);
    CHECK(differences == 0);
}
void Run(EffectsPixelDevice& backend, const DistortionOracle& geometry, uint8_t scale) {
    constexpr uint32_t height = 448;
    EffectsPixelFixture model(backend, scale, height), replacement(backend, scale, height);
    DistortionFixture input(height, replacement.frame.source_frame);
    DistortionMaterialOracle material;
    const uint64_t attributes = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 |
                                uint64_t{1} << 56 | uint64_t{1} << 59;
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        material.Reset({attributes | (scenario == 4 ? uint64_t{1} << 55 : 0),0,0}, height, 1);
        material.frame.environment = input.environment;
        material.target = {0,0,0,512,height,0,512,height};
        const auto source = material.Original();
        CHECK(source.size() == 2);
        input.view.distortion = scenario == 0 ? std::array<float,2>{0,0} :
            scenario == 1 ? std::array<float,2>{3,.75f} : std::array<float,2>{-3,-.75f};
        input.material.tex.mmin = scenario == 4;
        input.material.tex.mmag = input.material.tex.mmin;
        input.Build();
        std::vector<uint8_t> pixels(size_t(512 * scale) * height * scale * 4);
        for (uint32_t y = 0; y < height * scale; ++y) for (uint32_t x = 0; x < 512 * scale; ++x) {
            const auto at = (size_t(y) * 512 * scale + x) * 4;
            pixels[at] = uint8_t(x + scenario * 13); pixels[at+1] = uint8_t(y * 5 + scenario);
            pixels[at+2] = uint8_t((x/13 ^ y/7)*31); pixels[at+3] = uint8_t(32 + x % 160);
        }
        auto full_rect = source[0];
        // Common input setup only: create a full CT32 scene, then Initial()
        // uploads exactly the same authored pixel bytes into both fixtures.
        full_rect.raster.frame.psm = 0; full_rect.raster.frame.fbw = 512; full_rect.ctx = 0;
        for (auto& vertex : full_rect.verts) { vertex.x *= 2; vertex.y *= float(height) / 256; }
        const float depth = scenario == 3 ? 1.f : 0.f;
        model.Initial(full_rect, pixels, depth);
        replacement.Initial(full_rect, pixels, depth);
        backend.calls.clear(); backend.record = true;
        CHECK(model.pipeline.DrawGeneric(source[0]).drew);
        const auto batches = geometry.Run(input.blocks, input.look, input.view, input.material);
        for (const auto& batch : batches) {
            auto draw = source[1];
            draw.verts = batch.verts; draw.vertex_count = batch.vertex_count; draw.snapshot = batch.snapshot;
            CHECK(model.pipeline.DrawGeneric(draw).drew);
        }
        auto original_calls = std::move(backend.calls); backend.calls.clear();
        CHECK(replacement.effects.Distortion(replacement.frame, input.environment, input.draws) ==
              title::EffectsScene::Result::Ok);
        backend.record = false;
        CHECK(original_calls.size() == 5 && backend.calls.size() == original_calls.size());
        // The authored rectangle is 256 square. Its private alpha and padding
        // rows are not source texture inputs; compare every authored RGB byte.
        Compare(EffectsPixelDevice::CopyRgb(original_calls[0], 256, 256),
                EffectsPixelDevice::CopyRgb(backend.calls[0], 256, 256));
        for (size_t i = 0; i < original_calls.size(); ++i) {
            const auto& a = original_calls[i]; const auto& b = backend.calls[i];
            CHECK(a.parameters.render_w == b.parameters.render_w &&
                  a.parameters.render_h == b.parameters.render_h);
            CHECK(a.texture.transform.uv_tex_w == b.texture.transform.uv_tex_w &&
                  a.texture.transform.uv_tex_h == b.texture.transform.uv_tex_h);
            CHECK(a.draw.verts.size() == b.draw.verts.size());
            CHECK(ps2r::gs::GsdlFormat::VertexHash(a.draw.verts.data(), a.draw.verts.size()) ==
                  ps2r::gs::GsdlFormat::VertexHash(b.draw.verts.data(), b.draw.verts.size()));
        }
        const auto expected = model.Pixels(), actual = replacement.Pixels();
        if (expected != actual) std::fprintf(stderr, "distortion scale %u scenario %u\n", scale, scenario);
        Compare(expected, actual);
        CHECK(glGetError() == GL_NO_ERROR);
        if (scenario == 3) CHECK(actual == pixels);
        else CHECK(actual != pixels);
    }
    CHECK(replacement.effects.DistortionCounts().copies == 5 &&
          replacement.effects.DistortionCounts().batches == 20);
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::puts("SKIP: original static type18 image required"); return 77; }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        CHECK(bool(input));
        DistortionOracle oracle({std::istreambuf_iterator<char>(input), {}});
        ps2r::platform::Window window;
        ps2r::platform::GlContext context;
        if (!window.Create(true, 128, 64, "distortion interface self-oracle") || !context.Create(window)) return 77;
        EffectsPixelDevice device;
        CHECK(device.Init([&] { window.Swap(); }));
        Run(device, oracle, 1); Run(device, oracle, 2);
        std::printf("DistortionPixels: %u checks, real GL scales 1/2, same-input NEW producers/program/pipeline\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
