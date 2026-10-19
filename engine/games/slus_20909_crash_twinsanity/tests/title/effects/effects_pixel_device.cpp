#include "games/slus_20909_crash_twinsanity/tests/title/effects/effects_pixel_device.h"

#include <stdexcept>

std::vector<uint8_t> EffectsPixelDevice::CopyRgb(const Call& call, uint32_t width, uint32_t height) {
    const auto scale = call.parameters.scale;
    const auto stride = size_t(call.parameters.target_w * scale) * 4;
    if (width > uint32_t(call.parameters.target_w) || height > uint32_t(call.parameters.target_h) ||
        call.copy_pixels.size() < stride * height * scale)
        throw std::runtime_error("effect copy observation outside backing image");
    std::vector<uint8_t> rgb;
    rgb.reserve(size_t(width) * height * scale * scale * 3);
    for (uint32_t y = 0; y < height * scale; ++y)
        for (uint32_t x = 0; x < width * scale; ++x)
            for (unsigned channel = 0; channel < 3; ++channel)
                rgb.push_back(call.copy_pixels[y * stride + x * 4 + channel]);
    return rgb;
}
ps2r::render::DrawResult EffectsPixelDevice::Draw(const ps2r::gs::DrawIr& draw,
    const ps2r::gs::TextureSource& texture, const ps2r::render::DrawParams& parameters) {
    const auto result = GlDevice::Draw(draw, texture, parameters);
    if (!record) return result;
    Call call{draw, texture, parameters, {}};
    call.parameters.cd_copy = nullptr; // no borrowed tracker survives the call
    if (draw.prim_type == 6) {
        const auto description = DescribeSurface(parameters.color);
        call.copy_pixels.resize(size_t(description.width) * description.height * 4);
        Readback(parameters.color, {0,0,int(description.width),int(description.height)},
            call.copy_pixels.data(), size_t(description.width) * 4);
    }
    calls.push_back(std::move(call));
    return result;
}
