#include "games/slus_20909_crash_twinsanity/tests/title/effects/distortion_fixture.h"

#include <cstring>
#include <stdexcept>

DistortionFixture::DistortionFixture(uint32_t height, uint32_t source_frame) : blocks(1) {
    look.corners = {{{.25f,0}, {.125f,.21650635f}, {-.125f,.21650635f},
                     {-.25f,0}, {-.125f,-.21650635f}, {.125f,-.21650635f}}};
    for (unsigned step = 0; step < look.shapes.size(); ++step) {
        auto& shape = look.shapes[step];
        shape = {-2000, -1000, 4000, 0, 2000, 1000};
        for (unsigned channel = 0; channel < 4; ++channel) {
            uint32_t bits; std::memcpy(&bits, &shape[channel], 4);
            bits = (bits & ~255u) | (channel == 3 ? 80 + step : 128);
            std::memcpy(&shape[channel], &bits, 4);
        }
    }
    view.height = height; view.time = 2; view.distortion = {3, .75f};
    view.screen = {{{16,0,0,0}, {0,16,0,0}, {0,0,16,0}, {32768,32768,40000,1}}};
    for (unsigned i = 0; i < 12; ++i)
        blocks[0][i] = {{float(int(i)-6)*17, float(int(i%3)-1)*3, 0}, 0, {}, 3};
    environment.colour_clamp = true;
    const title::MaterialState::Frame frame{512, height, 2, 168, environment};
    const std::array<uint32_t, 8> target{0, 0, 0, 512, height, 0, 512, height};
    const uint64_t settings = 1 | uint64_t{1} << 26 | uint64_t{3} << 21 |
                              uint64_t{1} << 56 | uint64_t{1} << 59;
    if (title::DistortionMaterial::Build({settings, 0, 0}, frame, target, source_frame, material) !=
        title::DistortionMaterial::Result::Ok) throw std::runtime_error("distortion fixture material");
    Build();
}
void DistortionFixture::Build() {
    if (Geometry::Build(blocks, look, view, material, draws) != Geometry::Result::Ok)
        throw std::runtime_error("distortion fixture geometry");
}
