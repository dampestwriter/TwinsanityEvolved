#include "engine/platform/host/platform.h"
#include PS2R_GL_HEADER
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "engine/platform/sdl/gl_context.h"
#include "engine/platform/sdl/window.h"
#include "engine/render/backends/gl/gl_device.h"
#include "games/slus_20909_crash_twinsanity/tests/title/effects/shadow_fixture.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)

uint8_t Pixel(const std::vector<uint8_t>& bytes, int width, int x, int y) {
    return bytes[(size_t(y) * width + x) * 4];
}
void Run(ps2r::render::GlDevice& device, uint8_t scale) {
    constexpr uint32_t width = 128, height = 64;
    ShadowFixture fixture(device, width, height, scale);
    title::ShadowRenderer renderer(device, fixture.resources);
    const title::ShadowDraw::Frame frame{0, width, height};
    const ps2r::render::Rect full{0, 0, int(width * scale), int(height * scale)};
    const auto draw = ShadowFixture::Volume(width / 2, height / 2, 8, 3221225472.f);
    const auto back = ShadowFixture::Volume(width / 2, height / 2, 248, 3221225472.f);
    const auto rejected = ShadowFixture::Volume(width / 2, height / 2, 8, 0);
    const auto initial = [&](uint8_t alpha, float depth) {
        device.Clear(fixture.images[0], fixture.images[1], uint32_t(alpha) << 24 | 0xc8c8c8, depth,
                     ps2r::render::kClearColor | ps2r::render::kClearDepth);
    };
    const auto render = [&](const std::vector<ps2r::gs::DrawIr>& volumes) {
        CHECK(renderer.Draw(frame, volumes, fixture.scene) == title::ShadowRenderer::Result::Ok);
        std::vector<uint8_t> bytes(size_t(full.w) * full.h * 4);
        device.Readback(fixture.images[0], full, bytes.data(), size_t(full.w) * 4);
        CHECK(glGetError() == GL_NO_ERROR);
        return bytes;
    };
    initial(0, .5f);
    const auto blank = render({});
    CHECK(Pixel(blank, full.w, 16 * scale, 16 * scale) == 0xc8);
    initial(0, .5f);
    const auto front = render({draw});
    // OLD SHADOW2: 0x78 + 8 -> bit4 set -> 0x84; scene C8 - 84/4 -> A7.
    CHECK(Pixel(front, full.w, 16 * scale, 16 * scale) == 0xa7);
    CHECK(Pixel(front, full.w, 112 * scale, 48 * scale) == 0xc8);
    initial(0, .5f);
    const auto balanced = render({draw, back});
    CHECK(balanced == blank);
    initial(255, .5f);
    const auto blocked = render({draw});
    // The served scene's alpha gates the volume before it changes the counter.
    CHECK(Pixel(blocked, full.w, 16 * scale, 16 * scale) == 0xc8);
    initial(0, .5f);
    CHECK(render({rejected}) == blank);
    // A duplicated triangle in the SAME original strip counts only once.
    auto duplicate = draw;
    duplicate.verts.insert(duplicate.verts.end(), draw.verts.begin(), draw.verts.end());
    duplicate.vertex_count = uint32_t(duplicate.verts.size());
    initial(0, .5f);
    CHECK(render({duplicate}) == front);
    // Wrap through all 32 counter states then add one: no sticky parity latch.
    initial(0, .5f);
    CHECK(render(std::vector<ps2r::gs::DrawIr>(33, draw)) == front);
    // A following pass must see its own freshly supplied depth/color/alpha,
    // even when dimensions and private allocations are reused.
    initial(0, 0);
    CHECK(render({draw}) == blank);
    CHECK(renderer.Counts().passes == 8 && fixture.publications == 8);
    // Placement (play-session s4: no footprint at video.scale=2): a volume over
    // the half-size pixels x 40..60, y 20..30 darkens the scene at logical
    // (90, 44) -- its own place -- at every scale, and nowhere else: not at
    // (45, 22), where a pass rasterised native into a SCALED accumulator put it
    // (half size, in the top-left quarter: the s4 bug), and over exactly its
    // own area (the dark pixel count scales with the scene).
    ps2r::gs::DrawIr corner = draw;
    {
        ps2r::gs::VertexRec a = corner.verts[0], b = corner.verts[1], c = corner.verts[2];
        a.x = 40; a.y = 20; b.x = 60; b.y = 20; c.x = 40; c.y = 30;
        corner.verts = {a, b, c};
    }
    initial(0, .5f);
    const auto placed = render({corner});
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, dark = 0;
    for (int y = 0; y < full.h; ++y)
        for (int x = 0; x < full.w; ++x)
            if (Pixel(placed, full.w, x, y) != 0xc8) {
                ++dark;
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    std::printf("Shadow pixels scale %u: placed volume darkens %d pixels, x %d..%d y %d..%d\n",
                scale, dark, x0, x1, y0, y1);
    CHECK(Pixel(placed, full.w, 90 * scale, 44 * scale) == 0xa7);
    CHECK(Pixel(placed, full.w, 45 * scale, 22 * scale) == 0xc8);
    CHECK(Pixel(placed, full.w, 16 * scale, 16 * scale) == 0xc8);
    CHECK(x0 >= 78 * scale && x1 <= 121 * scale && y0 >= 38 * scale && y1 <= 61 * scale);
    CHECK(dark >= 500 * scale * scale && dark <= 560 * scale * scale);
    std::printf("Shadow pixels scale %u: %d checks so far, %d failures\n", scale, checks, failures);
}
}

int main() {
    ps2r::platform::Window window;
    ps2r::platform::GlContext context;
    if (!window.Create(true, 128, 64, "shadow interface pins") || !context.Create(window)) {
        std::printf("SKIP: no isolated GL context\n");
        return 77;
    }
    ps2r::render::GlDevice device;
    CHECK(device.Init([&] { window.Swap(); }));
    Run(device, 1);
    Run(device, 2);
    std::printf("ShadowPixels: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
