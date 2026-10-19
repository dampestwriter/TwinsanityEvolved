#include "games/slus_20909_crash_twinsanity/hle/graphics/display_layout.h"

#include <array>

namespace title {
bool DisplayLayout::Validate(const Output& out) {
    return out.width >= 64 && out.width <= 1024 && out.width % 64 == 0 &&
        out.height > 0 && out.height <= 1024 && uint64_t(out.width)*out.height*10 <= 4*1024*1024;
}

DisplayLayout::Result DisplayLayout::Capture(const MaterialState::Read& read, uint32_t entry, Output& out) {
    if (entry && entry != kAddress) return Result::UnsupportedEntry;
    std::array<uint32_t,22> settings{};
    std::array<uint32_t,2> dimensions{};
    try {
        if (!read || !read(kSettings,settings.data(),sizeof(settings)) ||
            !read(MaterialState::kDisplayWidth,dimensions.data(),sizeof(dimensions)))
            return Result::InvalidMemory;
    } catch (...) { return Result::InvalidMemory; }
    const Output next{dimensions[0],dimensions[1]};
    if (!Validate(next) ||
        settings[0] != 0 || settings[1] != next.width/64 || settings[2] != 2 ||
        settings[7] != 128 || settings[8] != 1 || settings[9] != 1 || settings[10] != 1 ||
        settings[11] || settings[12] || settings[13] ||
        !settings[16] || settings[16] > 15 || settings[17] ||
        settings[18] != uint64_t(settings[16]+1)*next.width || settings[18] > 4095 ||
        settings[19] != next.height-1 || settings[20] || settings[21])
        return Result::UnsupportedLayout;
    // Source width is in TV clocks, height is pixels-minus-one. The complete
    // image's logical dimensions are explicit above, independent of upscale.
    out = next;
    return Result::Ok;
}
} // namespace title
