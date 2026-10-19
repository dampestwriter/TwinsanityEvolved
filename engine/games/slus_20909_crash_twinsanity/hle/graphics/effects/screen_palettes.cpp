#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/screen_palettes.h"

#include <cstring>

namespace title {
namespace {
template<class T> bool ReadAt(const MaterialState::Read& read, uint32_t address, T& out) {
    return read && address <= 0x02000000 - sizeof(T) && read(address, &out, sizeof(T));
}
bool ReadTable(const MaterialState::Read& read, uint32_t index, ScreenPalettes::Palette& out) {
    return ReadAt(read, ScreenPalettes::kTables + index * ScreenPalettes::kStride, out);
}
}
void ScreenPalettes::Blend(const std::array<uint8_t, 3>& from, const std::array<uint8_t, 3>& to, float amount,
                           const Palette& alphas, Palette& out) {
    const float rest = 1.0f - amount;
    uint32_t colour = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const float blended = float(from[channel]) * amount + rest * float(to[channel]);
        // EE cvt.w.s truncates toward zero; the source then clamps to a byte.
        int32_t value = blended >= 2147483648.0f ? 0x7fffffff :
                        blended < -2147483648.0f ? int32_t(0x80000000) : int32_t(blended);
        if (value > 0xff) value = 0xff;
        if (value < 0) value = 0;
        colour |= uint32_t(value) << (channel * 8);
    }
    for (size_t index = 0; index < out.size(); ++index) out[index] = (alphas[index] & 0xff000000u) | colour;
}
void ScreenPalettes::Logical(const Palette& uploaded, Palette& out) {
    Palette logical;
    for (uint32_t index = 0; index < logical.size(); ++index) logical[index] = uploaded[Slot(index)];
    out = logical;
}
ScreenPalettes::Result ScreenPalettes::Resolve(const Read& read, uint32_t palette, uint32_t second,
                                               bool blends, float amount, Palette& out) {
    uint8_t on = 0;
    if (!ReadAt(read, kOn, on)) return Result::InvalidMemory;
    if (!on) return Result::Disabled;
    if (palette >= kCount || (blends && second >= kCount)) return Result::UnsupportedSource;
    // The first-colour pointers must still name the authored tables.
    std::array<uint32_t, kCount> pointers{};
    if (!ReadAt(read, kFirstColours, pointers)) return Result::InvalidMemory;
    for (uint32_t index = 0; index < kCount; ++index)
        if (pointers[index] != kTables + index * kStride) return Result::UnsupportedSource;
    Palette value;
    if (!blends) {
        if (!ReadTable(read, palette, value)) return Result::InvalidMemory;
        out = value;
        return Result::Ok;
    }
    Palette alphas;
    std::array<uint8_t, 3> from{}, to{};
    if (!ReadTable(read, 0, alphas) || !ReadAt(read, pointers[palette], from) || !ReadAt(read, pointers[second], to))
        return Result::InvalidMemory;
    Blend(from, to, amount, alphas, value);
    out = value;
    return Result::Ok;
}
}  // namespace title
