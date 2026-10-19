#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_texture.h"

#include <array>
#include <utility>

#include "engine/gs/format/clut_layout.h"
#include "engine/gs/format/pixel_convert.h"
#include "engine/gs/format/psm_layout.h"

namespace title {
namespace {
uint32_t Word(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Wide(const uint8_t* p) { return Word(p) | uint64_t(Word(p + 4)) << 32; }
using Layout = ps2r::gs::PsmLayout;
using Format = ps2r::gs::Psm;
const std::array<uint16_t, 2048>& FileOrder() {
    // Inverse of ONE authored 64x32 storage tile. Values are file word
    // positions, not a persistent memory image or address-to-resource map.
    static const auto order = [] {
        std::array<uint16_t, 2048> result{};
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                result[Layout::ElementInPage(Format::CT32, x, y)] = uint16_t(y * 64 + x);
        return result;
    }();
    return order;
}
}  // namespace

namespace {
// The family's header: a 32..128 px indexed (T8) image of 3..5 authored
// levels at storage base 0 inside a 64/128 px wide buffer, palette slot <= 128.
SkyTexture::Result CheckHeader(const uint8_t* h, size_t header_size,
                               uint32_t& wp, uint32_t& hp, uint32_t& base, uint32_t& width, uint32_t& palette) {
    if (!h || header_size < 0x60) return SkyTexture::Result::InvalidAsset;
    wp = uint32_t(h[4]) | uint32_t(h[5]) << 8;
    hp = uint32_t(h[6]) | uint32_t(h[7]) << 8;
    if (wp < 5 || wp > 7 || hp < 5 || hp > 7 || h[8] < 3 || h[8] > 5 ||
        h[9] != 0x13 || h[10] != 0 || h[11] > 1 || h[13] > 3)
        return SkyTexture::Result::UnsupportedFormat;
    base = Word(h + 0x10); width = Word(h + 0x2c); palette = Word(h + 0x48);
    if (base != 0 || (width != 1 && width != 2) || palette > 128) return SkyTexture::Result::UnsupportedFormat;
    return SkyTexture::Result::Ok;
}
}  // namespace

SkyTexture::Result SkyTexture::FromConverted(const uint8_t* h, size_t header_size,
                                              const TextureCodec::Image& converted, Image& out) {
    uint32_t wp = 0, hp = 0, base = 0, width = 0, palette = 0;
    const auto header = CheckHeader(h, header_size, wp, hp, base, width, palette);
    if (header != Result::Ok) return header;
    if (converted.format != TextureCodec::Format::Indexed8 || converted.levels.empty() ||
        converted.levels.size() != h[8] || converted.alpha != bool(h[11]) || converted.function != h[13])
        return Result::UnsupportedFormat;
    const auto& level = converted.levels[0];
    if (level.width != (1u << wp) || level.height != (1u << hp) ||
        level.rgba.size() != size_t(level.width) * level.height * 4) return Result::InvalidAsset;
    Image image;
    image.width = level.width; image.height = level.height;
    image.buffer_width = width * 64;
    image.levels = h[8]; image.function = h[13]; image.alpha = h[11];
    image.rgba = level.rgba;
    out = std::move(image);
    return Result::Ok;
}

SkyTexture::Result SkyTexture::Decode(const uint8_t* h, size_t header_size,
                                       const uint8_t* storage, size_t size, Image& out) {
    uint32_t wp = 0, hp = 0, base = 0, width = 0, palette = 0;
    if (!storage || size < 96) return Result::InvalidAsset;
    const auto header = CheckHeader(h, header_size, wp, hp, base, width, palette);
    if (header != Result::Ok) return header;
    const uint32_t count = Word(storage) & 0xffff;
    const uint32_t image_width = Word(storage + 48), image_height = Word(storage + 52);
    const size_t image_size = size_t(image_width) * image_height * 4;
    // This family's complete authored record: one rectangle and its image.
    // An unclassified command, layout, or trailing byte declines atomically.
    if (Word(storage) != (0x60000000 | count) || Word(storage + 4) || Word(storage + 8) ||
        Word(storage + 12) != (0x50000000 | count) || size != 16 + size_t(count) * 16 ||
        Wide(storage + 16) != 0x1000000000000003ull || Wide(storage + 24) != 0xe ||
        Wide(storage + 32) != 0 || Wide(storage + 40) != 0x51 || Wide(storage + 56) != 0x52 ||
        Wide(storage + 64) != 0 || Wide(storage + 72) != 0x53 || Wide(storage + 88) != 0 ||
        image_width != 64 || (image_height != 32 && image_height != 64 && image_height != 96) ||
        size != 96 + image_size ||
        Wide(storage + 80) != (0x0800000000008000ull | uint64_t(image_size / 16)))
        return Result::InvalidAsset;
    const auto& order = FileOrder();
    const auto byte = [&](uint32_t address, uint8_t& result) {
        if (address >= image_size) return false;
        const size_t position = (address / 8192) * 8192 + order[(address % 8192) / 4] * 4 + address % 4;
        result = storage[96 + position];
        return true;
    };
    std::array<std::array<uint8_t, 4>, 256> colours{};
    for (uint32_t index = 0; index < colours.size(); ++index) {
        const uint32_t address = ps2r::gs::ClutLayout::EntryAddress(Format::CT32, palette, false, 256, index) * 4;
        for (unsigned lane = 0; lane != 4; ++lane)
            if (!byte(address + lane, colours[index][lane])) return Result::InvalidAsset;
        colours[index][3] = ps2r::gs::PixelConvert::ScaleAlphaPs2ToGpu(colours[index][3]);
    }
    Image image;
    image.width = 1u << wp; image.height = 1u << hp;
    image.buffer_width = width * 64;
    image.levels = h[8]; image.function = h[13]; image.alpha = h[11];
    image.rgba.reserve(size_t(image.width) * image.height * 4);
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x) {
            uint8_t index;
            if (!byte(Layout::PixelByteAddress(Format::T8, x, y, base, width), index))
                return Result::InvalidAsset;
            image.rgba.insert(image.rgba.end(), colours[index].begin(), colours[index].end());
        }
    out = std::move(image);
    return Result::Ok;
}

}  // namespace title
