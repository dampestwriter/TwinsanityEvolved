#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/strip_draw.h"

namespace title {

// Source-level state for the UI's flat Strip material. This reads title objects,
// never shader packets, GIF registers, the GS model, or a last-material cache.
class MaterialState {
public:
    // SLUS_209.09: OLEG::SetUpFlatSprites (0x16ce50), type-E vtable, and
    // SetUpRenderTarget (0x19ef90) / type-E settings (0x1d3f38) global loads.
    static constexpr uint32_t kFlatMaterial = 0x0030bb20;
    static constexpr uint32_t kFlatShaderVtable = 0x002f9db8;
    static constexpr uint32_t kRenderTarget = 0x0030983c;
    static constexpr uint32_t kDisplayWidth = 0x0030a764;
    static constexpr uint32_t kDisplayHeight = 0x0030a768;
    static constexpr uint32_t kDisplayPixelBytes = 0x00309830;
    static constexpr uint32_t kDepthBufferPage = 0x0030a780;

    using Read = std::function<bool(uint32_t, void*, size_t)>;
    using Coordinates = StripDraw::Target;
    enum class Result {
        Ok, InvalidMemory, UnsupportedMaterial, UnsupportedShader,
        UnsupportedTarget, UnsupportedFrame
    };

    // Owned by the native frame/ordered Graphics caller. Begin2D's shared call
    // and shader do NOT define these inherited settings. Supply the environment
    // established by the title's frame intent, not a snapshot of the GS model.
    // TR frame.cpp StartFrame/WriteFrameHead establishes colour clamp, disables
    // dithering after its copy, and installs the dither matrix; ordering must
    // also account for intervening effects. No implicit "known default" exists.
    struct Environment {
        bool colour_clamp = false;
        bool per_pixel_alpha = false;
        uint8_t scan_mask = 0;
        uint8_t fog_r = 0, fog_g = 0, fog_b = 0;
        uint8_t dither = 0;
        uint64_t dither_matrix = 0;
    };
    struct Frame {
        uint32_t display_width = 0, display_height = 0;
        uint32_t display_pixel_bytes = 0;
        uint32_t depth_buffer_page = 0;
        Environment environment;
    };
    struct Output {
        ps2r::gs::DrawIr draw;
        Coordinates coordinates;
        uint32_t bucket = 0;
    };

    // Read only the four software frame globals, with an explicit environment.
    // Neither this nor Build observes a packet allocation or publishes work.
    static Result ReadFrame(const Read& read, const Environment& environment,
                            Frame& out);

    // Accept only the retail flat-material object: one type-E shader, bucket 24,
    // mix blending, first context, untextured Gouraud, no fog/alpha/destination
    // tests, GEQUAL depth test and no depth writes. Other cases decline.
    // Unused settings bits are deliberately not required to be zero: the retail
    // constructor leaves some allocator contents, including the STQ bit, intact.
    //
    // target is the current RenderTargetDescription, not its cached packet.
    // Rectangles must be nonempty and fit the 11-bit scissor domain. The target's
    // display size determines geometry; the frame's size determines XYOFFSET.
    //
    // Success owns a complete untextured strip template, without vertices or an
    // invented producer identity. Caller supplies identity/geometry/submission.
    // All failures leave out unchanged. Memory reads must be stable during the
    // call and must not cause MMIO or execute guest code. No guest writes occur.
    static Result Build(const Read& read, uint32_t material, uint32_t target,
                        const Frame& frame, Output& out);
};

}  // namespace title
