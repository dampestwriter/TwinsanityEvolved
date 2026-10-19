#pragma once

#include <array>
#include <functional>

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_palettes.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/graphics_resources.h"

namespace title {

// The owned bucket-4 screen passes over the shared scene (TR
// renderer/effects.cpp DrawScreenEffects: CopyDepthBuffer, BlurFarPixels,
// FilterColours; WaveFrame is WaveRenderer's). Inputs are values captured at
// the source boundary; no source address, packet or register state is read.
//
// The filter is modelled by its INTENT, not by the retail VRAM aliasing: the
// depth copy writes a Z16 view of the Z32 buffer into a 16-bit frame under
// FBMSK 0x3FFF (the 5551 fold keeps bits 8-15 of the halfword), which the
// filter then reads as PSMT8H -- so every pixel's index is Z[15:8] and the
// GREATER test at 0x10000 admits exactly the pixels whose Z has no higher
// bit. The owned draw samples the scene depth through the backend's
// depth-byte palette serve (TextureSource fmt 7, shift 8) with the palette
// FilterColours would have uploaded, in logical index order.
class ScreenRenderer {
public:
    enum class Result { Ok, InvalidInput, InvalidScene, ResourceFailure,
                        BackendFailure, PublicationFailure, Faulted };
    struct Pass {
        uint32_t height = 0;                 // g_ScreenHeight; the width is always 512
        bool blur = false;                   // BlurFarPixels
        bool filter = false;                 // FilterColours with g_ColourFilterOn
        ScreenPalettes::Palette palette{};   // storage (CSM1) order, as the source uploads it
    };
    struct Scene {
        std::array<Graphics::Lease, 3> images; // color, depth, destination copy
        std::function<bool(const ps2r::gs::DrawIr&, const ps2r::render::DrawResult&)> publish;
    };
    struct Counters {
        uint64_t passes = 0, blur_passes = 0, filter_draws = 0, palette_uploads = 0;
        uint64_t allocations = 0, failures = 0;
    };
    static constexpr uint32_t kDepthShift = 8;
    // The CLUT rows the backend samples: row 0 with GPU-scaled alpha, row 1
    // raw, both in LOGICAL index order (ScreenPalettes::Logical applied once).
    static void ClutRows(const ScreenPalettes::Palette& storage, std::array<uint8_t, 256 * 2 * 4>& out);

    ScreenRenderer(ps2r::render::Backend&, GraphicsResources&);
    ~ScreenRenderer();
    // Ordered backend owner. Blur then filter, each publishing its scene write.
    Result Draw(uint32_t frame, const Pass&, const Scene&);
    void ResetAfterQuiescence();
    Counters Counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace title
