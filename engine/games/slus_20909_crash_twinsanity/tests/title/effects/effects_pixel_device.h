#pragma once

#include "engine/gs/hle/ir/draw_ir.h"
#include "engine/gs/hle/texsrc/texture_source.h"
#include "engine/render/backends/gl/gl_device.h"

// Test-only observation at the shared backend boundary. Both arms still call
// the same real GL implementation; no replacement values enter the NEW arm.
class EffectsPixelDevice : public ps2r::render::GlDevice {
public:
    struct Call {
        ps2r::gs::DrawIr draw;
        ps2r::gs::TextureSource texture;
        ps2r::render::DrawParams parameters;
        std::vector<uint8_t> copy_pixels;
    };
    bool record = false;
    std::vector<Call> calls;
    static std::vector<uint8_t> CopyRgb(const Call&, uint32_t width, uint32_t height);
    ps2r::render::DrawResult Draw(const ps2r::gs::DrawIr& draw,
        const ps2r::gs::TextureSource& texture, const ps2r::render::DrawParams& parameters) override;
};
