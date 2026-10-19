#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_dump.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "engine/core/image/png.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_pack.h"

namespace title {
namespace {
// Nearest-sampled square thumbnail of a level, letterboxed to kTile when the
// level is not square (the empty area stays transparent black).
std::vector<uint8_t> Thumb(const uint8_t* rgba, uint32_t width, uint32_t height) {
    constexpr uint32_t tile = TextureDump::kTile;
    std::vector<uint8_t> out(size_t(tile) * tile * 4, 0);
    const uint32_t longest = std::max(width, height);
    const uint32_t tw = std::max(1u, width * tile / longest), th = std::max(1u, height * tile / longest);
    const uint32_t ox = (tile - tw) / 2, oy = (tile - th) / 2;
    for (uint32_t y = 0; y < th; ++y)
        for (uint32_t x = 0; x < tw; ++x) {
            const uint32_t sx = std::min(width - 1, x * width / tw), sy = std::min(height - 1, y * height / th);
            const uint8_t* src = rgba + (size_t(sy) * width + sx) * 4;
            uint8_t* dst = out.data() + (size_t(oy + y) * tile + ox + x) * 4;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;   // the sheet is opaque
        }
    return out;
}
// 3x5 hex glyphs, rows top to bottom, 3 bits each (MSB = left).
constexpr uint8_t kGlyphs[16][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},   // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},   // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},   // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},   // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},   // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},   // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},   // 6
    {0b111, 0b001, 0b001, 0b001, 0b001},   // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},   // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},   // 9
    {0b010, 0b101, 0b111, 0b101, 0b101},   // a
    {0b110, 0b101, 0b110, 0b101, 0b110},   // b
    {0b111, 0b100, 0b100, 0b100, 0b111},   // c
    {0b110, 0b101, 0b101, 0b101, 0b110},   // d
    {0b111, 0b100, 0b111, 0b100, 0b111},   // e
    {0b111, 0b100, 0b111, 0b100, 0b100},   // f
};
}  // namespace

const uint8_t* TextureDump::Glyph(unsigned digit) { return kGlyphs[digit & 15]; }

bool TextureDump::Open(const std::string& directory, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code code;
    if (directory.empty() || (!std::filesystem::create_directories(directory, code) &&
                              !std::filesystem::is_directory(directory, code))) {
        if (error) *error = "cannot create the texture dump directory: " + directory;
        return false;
    }
    directory_ = directory;
    index_ = "hash\twidth\theight\tlevels\tformat\tasset_id\n";
    if (error) error->clear();
    return true;
}

void TextureDump::Record(const TextureCodec::Image& image, uint32_t asset_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (directory_.empty() || finished_ || image.levels.empty()) return;
    ++counts_.records;
    if (!seen_.insert(image.content_hash).second) return;
    const auto& base = image.levels[0];
    const std::string hex = TexturePack::HexOf(image.content_hash);
    std::string error;
    if (ps2r::image::Png::EncodeFile(directory_ + "/" + hex + ".png", base.rgba.data(), base.width, base.height, &error))
        ++counts_.written;
    else
        ++counts_.failures;
    char line[160];
    std::snprintf(line, sizeof(line), "%s\t%u\t%u\t%zu\t%s\t%08x\n", hex.c_str(), base.width, base.height,
                  image.levels.size(), image.format == TextureCodec::Format::Indexed8 ? "indexed8" : "rgba8", asset_id);
    index_ += line;
    entries_.push_back({image.content_hash, base.width, base.height, Thumb(base.rgba.data(), base.width, base.height)});
}

void TextureDump::ContactSheet(const std::vector<Thumbnail>& thumbnails, uint32_t& width, uint32_t& height,
                               std::vector<uint8_t>& rgba) {
    const uint32_t count = uint32_t(thumbnails.size());
    const uint32_t columns = std::min(kColumns, std::max(1u, count));
    const uint32_t rows = count ? (count + columns - 1) / columns : 1;
    width = columns * kTile;
    height = rows * (kTile + kLabel);
    rgba.assign(size_t(width) * height * 4, 0);
    for (size_t y = 3; y < rgba.size(); y += 4) rgba[y] = 255;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& t = thumbnails[i];
        const uint32_t cx = (i % columns) * kTile, cy = (i / columns) * (kTile + kLabel);
        const auto thumb = Thumb(t.rgba->data(), t.width, t.height);
        for (uint32_t y = 0; y < kTile; ++y)
            std::copy_n(thumb.data() + size_t(y) * kTile * 4, size_t(kTile) * 4,
                        rgba.data() + (size_t(cy + y) * width + cx) * 4);
        // The 16 hex digits, 4 px per glyph cell (3 + 1 gap), 5 rows + 1 gap, white on black.
        const std::string hex = TexturePack::HexOf(t.hash);
        for (uint32_t d = 0; d < 16; ++d) {
            const unsigned digit = hex[d] <= '9' ? unsigned(hex[d] - '0') : unsigned(hex[d] - 'a') + 10;
            const uint8_t* glyph = Glyph(digit);
            for (uint32_t gy = 0; gy < 5; ++gy)
                for (uint32_t gx = 0; gx < 3; ++gx) {
                    if (!(glyph[gy] >> (2 - gx) & 1)) continue;
                    uint8_t* px = rgba.data() + (size_t(cy + kTile + 1 + gy) * width + cx + d * 4 + gx) * 4;
                    px[0] = px[1] = px[2] = 255;
                }
        }
    }
}

void TextureDump::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (directory_.empty() || finished_) return;
    finished_ = true;
    std::ofstream index(directory_ + "/index.tsv", std::ios::binary | std::ios::trunc);
    index << index_;
    std::vector<Thumbnail> thumbnails;
    thumbnails.reserve(entries_.size());
    for (const auto& e : entries_) thumbnails.push_back({e.hash, kTile, kTile, &e.thumb});
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    ContactSheet(thumbnails, width, height, rgba);
    std::string error;
    if (!ps2r::image::Png::EncodeFile(directory_ + "/contact_sheet.png", rgba.data(), width, height, &error))
        ++counts_.failures;
}

TextureDump::Counters TextureDump::Counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_;
}

}  // namespace title
