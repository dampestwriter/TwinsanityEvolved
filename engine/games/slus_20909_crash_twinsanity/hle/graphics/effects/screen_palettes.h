#pragma once

#include <array>

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// The colour-filter palettes FilterColours uploads (TR renderer/effects.cpp:
// MakeFilterPalettes, BlendPalettes, FilterColours). The eight authored
// palettes are read-only title data; the blended palette is a pure function of
// two of them and the amount, with the retail behaviour kept: every entry is
// made from the two palettes' FIRST colours (one flat colour) and keeps the
// alpha the blended buffer was initialised with (palette 0's). An owned filter
// pass takes these values at the DrawScreenEffects call instead of reading the
// model's upload packets or its alternating blend buffers.
class ScreenPalettes {
public:
    using Read = MaterialState::Read;
    using Palette = std::array<uint32_t, 256>;
    static constexpr uint32_t kCount = 8, kStride = 0x400;
    static constexpr uint32_t kTables = 0x002f6c78;       // g_FilterPalettes[8][256]
    static constexpr uint32_t kFirstColours = 0x002e7008; // g_FilterPaletteColours[8]
    static constexpr uint32_t kOn = 0x00309891;           // g_ColourFilterOn
    enum class Result { Ok, InvalidMemory, UnsupportedSource, Disabled };
    // The palette FilterColours would upload for these arguments: the authored
    // palette, or the blend of the two palettes' first colours by `amount`
    // (truncated, clamped 0..255 per channel, palette 0's alpha per entry).
    static Result Resolve(const Read&, uint32_t palette, uint32_t second, bool blends, float amount, Palette& out);
    // Pure blend of two first colours (RGB bytes) with palette 0's alphas; the
    // arithmetic order is the source's (from*amount + (1-amount)*to).
    static void Blend(const std::array<uint8_t, 3>& from, const std::array<uint8_t, 3>& to, float amount,
                      const Palette& alphas, Palette& out);
    // The GS CSM1 storage slot of a logical 8-bit index (TR renderer.h:254,
    // PaletteSlot: bits 3 and 4 swapped — of every 32 entries, 8-15 and 16-23
    // change places). The authored tables and every upload (identity, filter,
    // blended, shadow depth) are in storage order; an owned pass that samples
    // the frame's alpha byte as index i takes entry Slot(i). The map is its
    // own inverse and is applied exactly once, at the lookup.
    static constexpr uint32_t Slot(uint32_t index) {
        return (index & 0xe7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
    }
    // The uploaded palette in logical index order: out[i] is the colour the
    // GS returns for index i. Safe when `uploaded` and `out` alias.
    static void Logical(const Palette& uploaded, Palette& out);
};

}  // namespace title
