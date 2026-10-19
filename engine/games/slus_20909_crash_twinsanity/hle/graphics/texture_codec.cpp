#include "games/slus_20909_crash_twinsanity/hle/graphics/texture_codec.h"

#include <algorithm>
#include <utility>

#include "engine/core/hash/hash.h"
#include "engine/gs/format/clut_layout.h"
#include "engine/gs/format/pixel_convert.h"
#include "engine/gs/format/psm_layout.h"

namespace title {
namespace {
using Result = TextureCodec::Result;
using Layout = ps2r::gs::PsmLayout;
using StorageFormat = ps2r::gs::Psm;
uint16_t Half(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t Word(const uint8_t* p) { return Half(p) | (uint32_t(Half(p + 2)) << 16); }
uint64_t Wide(const uint8_t* p) { return Word(p) | (uint64_t(Word(p + 4)) << 32); }
Result Fail(Result result, std::string* error, const char* why) {
    if (error) *error = std::string("Texture: ") + why;
    return result;
}
const std::array<uint16_t, 2048>& TileOrder() {
    // Inverse word order of one authored 64x32 RGBA storage tile. Reuse pure
    // format arithmetic; this is an asset-byte permutation, not mutable RAM.
    static const auto order = [] {
        std::array<uint16_t, 2048> map{};
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                map[Layout::ElementInPage(StorageFormat::CT32, x, y)] = uint16_t(y * 64 + x);
        return map;
    }();
    return order;
}
bool AssetByte(const uint8_t* pixels, size_t size, uint32_t address, uint8_t& out) {
    if (address >= size) return false;
    const size_t offset = size_t(address / 8192) * 8192 +
        size_t(TileOrder()[(address % 8192) / 4]) * 4 + address % 4;
    if (offset >= size) return false;
    out = pixels[offset];
    return true;
}
bool FixedEnvelope(const uint8_t* storage, size_t size, uint32_t width, uint32_t height) {
    const uint64_t bytes = uint64_t(width) * height * 4;
    if (!width || !height || bytes > TextureCodec::kMaxRecordBytes ||
        bytes % 16 || bytes / 16 > 0x7fff || size != 96 + bytes) return false;
    const auto count = uint32_t(bytes / 16 + 5);
    // All currently declared asset records carry this exact file wrapper:
    // fixed one-image schema, no branches, external references or extra writes.
    return Word(storage) == (0x60000000 | count) && Word(storage + 4) == 0 &&
        Word(storage + 8) == 0 && Word(storage + 12) == (0x50000000 | count) &&
        Wide(storage + 16) == 0x1000000000000003ull && Wide(storage + 24) == 0xe &&
        Wide(storage + 32) == 0 && Wide(storage + 40) == 0x51 &&
        Wide(storage + 56) == 0x52 && Wide(storage + 64) == 0 &&
        Wide(storage + 72) == 0x53 && Wide(storage + 88) == 0 &&
        Wide(storage + 80) == (0x0800000000008000ull | (bytes / 16));
}
bool IndexedLevel(const uint8_t* h, unsigned index, const uint8_t* pixels, size_t size,
                  const TextureCodec::Image& image, TextureCodec::Level& level) {
    const uint32_t base = Word(h + 0x10 + index * 4);
    const uint32_t width = Word(h + 0x2c + index * 4);
    // Bound the byte arithmetic BEFORE the shared format helper's wrap rule.
    // These asset footprints may not wrap or read another texture's storage.
    if (!width || width > 16 || uint64_t(base) * 256 >= size ||
        uint64_t(base) * 256 + uint64_t((level.height + 63) / 64) *
            std::max(1u, width / 2) * 8192 > Layout::kVramBytes) return false;
    level.rgba.resize(size_t(level.width) * level.height * 4);
    for (uint32_t y = 0; y < level.height; ++y)
        for (uint32_t x = 0; x < level.width; ++x) {
            uint8_t colour;
            if (!AssetByte(pixels, size, Layout::PixelByteAddress(StorageFormat::T8, x, y, base, width), colour))
                return false;
            const auto& rgba = image.palette[colour];
            std::copy(rgba.begin(), rgba.end(), level.rgba.begin() + (size_t(y) * level.width + x) * 4);
        }
    return true;
}
}

TextureCodec::Result TextureCodec::Decode(const uint8_t* record, size_t size, Image& out, std::string* error) {
    if (!record || size < 4 + kHeaderBytes + kToolsBytes)
        return Fail(Result::InvalidAsset, error, "truncated texture record");
    if (size > kMaxRecordBytes) return Fail(Result::LimitExceeded, error, "record byte limit exceeded");
    if (Word(record) != size - 4) return Fail(Result::InvalidAsset, error, "declared size disagrees with record extent");
    return DecodeParts(record + 4, kHeaderBytes, record + 4 + kHeaderBytes + kToolsBytes,
                       size - 4 - kHeaderBytes - kToolsBytes, out, error);
}

TextureCodec::Result TextureCodec::DecodeParts(const uint8_t* h, size_t header_size,
    const uint8_t* storage, size_t size, Image& out, std::string* error) {
    if (!h || header_size != kHeaderBytes || !storage || size < 96)
        return Fail(Result::InvalidAsset, error, "missing complete header or storage image");
    if (size > kMaxRecordBytes) return Fail(Result::LimitExceeded, error, "storage byte limit exceeded");
    const uint32_t wp = Half(h + 4), hp = Half(h + 6);
    if (wp > 10 || hp > 10 || h[8] == 0 || h[8] > 7 || h[11] > 1 || h[13] > 3)
        return Fail(Result::InvalidAsset, error, "invalid dimensions, level count or colour metadata");
    if ((h[9] != 0 && h[9] != 0x13) || h[10] != 0)
        return Fail(Result::UnsupportedFormat, error, "asset pixel/storage format is not source-classified");
    const uint32_t storage_width = Word(storage + 48), storage_height = Word(storage + 52);
    if (!FixedEnvelope(storage, size, storage_width, storage_height))
        return Fail(Result::UnsupportedLayout, error, "storage does not match the fixed one-image asset schema");
    Image image;
    image.format = h[9] == 0 ? Format::Rgba8 : Format::Indexed8;
    image.alpha = h[11] != 0;
    image.function = h[13];
    const uint8_t* pixels = storage + 96;
    const size_t bytes = size - 96;
    if (image.format == Format::Rgba8) {
        if (h[8] != 1 || Word(h + 0x10) != 0 || storage_width != (1u << wp) ||
            storage_height != (1u << hp) || Word(h + 0x2c) * uint64_t{64} != storage_width)
            return Fail(Result::UnsupportedLayout, error, "direct image dimensions or mip layout is not classified");
        image.levels.push_back({storage_width, storage_height, {pixels, pixels + bytes}});
        for (size_t i = 3; i < bytes; i += 4)
            image.levels[0].rgba[i] = ps2r::gs::PixelConvert::ScaleAlphaPs2ToGpu(pixels[i]);
    } else {
        if (storage_width != 64 || storage_height % 32 != 0)
            return Fail(Result::UnsupportedLayout, error, "indexed storage must contain complete 64x32 tiles");
        const uint32_t palette = Word(h + 0x48);
        if (uint64_t(palette) * 256 + 1024 > bytes)
            return Fail(Result::InvalidAsset, error, "palette exceeds the owned storage image");
        image.palette.resize(256);
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t address = ps2r::gs::ClutLayout::EntryAddress(StorageFormat::CT32, palette, false, 256, i) * 4;
            for (unsigned c = 0; c < 4; ++c)
                if (!AssetByte(pixels, bytes, address + c, image.palette[i][c]))
                    return Fail(Result::InvalidAsset, error, "palette tile exceeds the owned storage image");
            image.palette[i][3] = ps2r::gs::PixelConvert::ScaleAlphaPs2ToGpu(image.palette[i][3]);
        }
        uint64_t count = 0;
        for (unsigned i = 0; i < h[8]; ++i) {
            Level level{std::max(1u, (1u << wp) >> i), std::max(1u, (1u << hp) >> i), {}};
            count += uint64_t(level.width) * level.height;
            if (count > kMaxPixels) return Fail(Result::LimitExceeded, error, "decoded pixel limit exceeded");
            if (!IndexedLevel(h, i, pixels, bytes, image, level))
                return Fail(Result::InvalidAsset, error, "mip footprint exceeds the owned storage image");
            image.levels.push_back(std::move(level));
        }
    }
    image.content_hash = ContentHash(image);
    out = std::move(image);
    if (error) error->clear();
    return Result::Ok;
}

uint64_t TextureCodec::ContentHash(const Image& image) {
    // Chained xxHash64: each step seeds the next, so the key depends on the
    // level order and dimensions as well as the bytes.
    uint64_t h = ps2r::hash::Xxh64(nullptr, 0, uint64_t(image.levels.size()));
    for (const auto& level : image.levels) {
        const uint32_t dims[2] = {level.width, level.height};
        h = ps2r::hash::Xxh64(dims, sizeof(dims), h);
        h = ps2r::hash::Xxh64(level.rgba.data(), level.rgba.size(), h);
    }
    return h;
}

bool TextureCodec::ApplyReplacement(Image& image, uint32_t width, uint32_t height, std::vector<uint8_t> rgba) {
    if (image.levels.empty() || !width || !height || rgba.size() != size_t(width) * height * 4) return false;
    std::vector<Level> chain;
    chain.push_back({width, height, std::move(rgba)});
    while (chain.size() < image.levels.size()) {
        const Level& src = chain.back();
        Level next{std::max(1u, src.width / 2), std::max(1u, src.height / 2), {}};
        next.rgba.resize(size_t(next.width) * next.height * 4);
        for (uint32_t y = 0; y < next.height; ++y)
            for (uint32_t x = 0; x < next.width; ++x)
                for (unsigned c = 0; c < 4; ++c) {
                    unsigned sum = 0, n = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint32_t sx = std::min(src.width - 1, x * 2 + dx), sy = std::min(src.height - 1, y * 2 + dy);
                            sum += src.rgba[(size_t(sy) * src.width + sx) * 4 + c];
                            ++n;
                        }
                    next.rgba[(size_t(y) * next.width + x) * 4 + c] = uint8_t((sum + n / 2) / n);
                }
        chain.push_back(std::move(next));
    }
    image.pack_levels = std::move(chain);
    return true;
}

}  // namespace title
