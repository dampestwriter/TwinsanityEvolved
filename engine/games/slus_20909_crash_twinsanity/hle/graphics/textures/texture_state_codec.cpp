#include "games/slus_20909_crash_twinsanity/hle/graphics/textures/texture_state_codec.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace title {
namespace {
bool Bad(std::string* error, const char* why) {
    if (error) *error = std::string("Texture state: ") + why;
    return false;
}
bool Power(uint32_t value) { return value && value <= 1024 && (value & (value - 1)) == 0; }
void Put(std::vector<uint8_t>& bytes, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}
bool ImageSize(const TextureCodec::Image& image, uint64_t& size) {
    const bool indexed = image.format == TextureCodec::Format::Indexed8;
    if ((!indexed && image.format != TextureCodec::Format::Rgba8) || image.function > 3 ||
        image.levels.empty() || image.levels.size() > 7 || (!indexed && image.levels.size() != 1) ||
        image.palette.size() != (indexed ? 256u : 0u)) return false;
    const auto& first = image.levels.front();
    if (!Power(first.width) || !Power(first.height)) return false;
    uint64_t pixels = 0;
    for (size_t i = 0; i < image.levels.size(); ++i) {
        const auto& level = image.levels[i];
        if (level.width != std::max(1u, first.width >> i) ||
            level.height != std::max(1u, first.height >> i) ||
            level.rgba.size() != uint64_t(level.width) * level.height * 4) return false;
        pixels += uint64_t(level.width) * level.height;
    }
    if (pixels > TextureCodec::kMaxPixels) return false;
    size = pixels * 4 + image.palette.size() * 4;
    return true;
}
}

bool TextureAssets::ValidateSnapshot(const Snapshot& snapshot, Limits limits, std::string* error) {
    if (snapshot.version != 1) return Bad(error, "unsupported owner version");
    if (snapshot.assets.size() > limits.versions) return Bad(error, "version count exceeds owner limit");
    std::unordered_set<uint32_t> objects;
    uint64_t generation = 0, bytes = 0;
    for (const auto& asset : snapshot.assets) {
        if (!asset.token.object || (asset.token.object & 3) ||
            !objects.insert(asset.token.object).second) return Bad(error, "invalid or repeated object identity");
        if (asset.token.generation <= generation ||
            (snapshot.next_generation && asset.token.generation >= snapshot.next_generation))
            return Bad(error, "invalid, repeated or unordered generation");
        generation = asset.token.generation;
        uint64_t size = 0;
        if (!ImageSize(asset.image, size)) return Bad(error, "invalid converted image/mip/palette");
        if (size > limits.bytes || bytes > limits.bytes - size)
            return Bad(error, "owned byte limit exceeded");
        bytes += size;
    }
    if (error) error->clear();
    return true;
}

struct TextureStateCodec::Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    uint64_t Read(unsigned width, bool& ok) {
        if (!ok || width > bytes.size() - at) { ok = false; return 0; }
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[at++]) << (i * 8);
        return value;
    }
    bool Raw(uint8_t* out, size_t size) {
        if (size > bytes.size() - at) return false;
        std::copy_n(bytes.data() + at, size, out);
        at += size;
        return true;
    }
};

bool TextureStateCodec::Encode(const TextureAssets::Snapshot& snapshot, std::vector<uint8_t>& out,
                               std::string* error) {
    if (!TextureAssets::ValidateSnapshot(snapshot, TextureAssets::Limits{}, error)) return false;
    uint64_t size = 20;
    for (const auto& asset : snapshot.assets) {
        size += 24 + asset.image.palette.size() * 4;
        for (const auto& level : asset.image.levels) size += 12 + level.rgba.size();
    }
    if (size > kMaxPayloadBytes) return Bad(error, "payload exceeds state-file section limit");
    std::vector<uint8_t> bytes;
    bytes.reserve(size_t(size));
    Put(bytes, kTag, 4); Put(bytes, kVersion, 4);
    Put(bytes, snapshot.next_generation, 8); Put(bytes, snapshot.assets.size(), 4);
    for (const auto& asset : snapshot.assets) {
        Put(bytes, asset.token.object, 4); Put(bytes, asset.asset_id, 4); Put(bytes, asset.token.generation, 8);
        const auto& image = asset.image;
        Put(bytes, uint8_t(image.format), 1); Put(bytes, image.alpha ? 1 : 0, 1);
        Put(bytes, image.function, 1); Put(bytes, image.levels.size(), 1);
        Put(bytes, image.palette.size(), 4);
        for (const auto& colour : image.palette) bytes.insert(bytes.end(), colour.begin(), colour.end());
        for (const auto& level : image.levels) {
            Put(bytes, level.width, 4); Put(bytes, level.height, 4); Put(bytes, level.rgba.size(), 4);
            bytes.insert(bytes.end(), level.rgba.begin(), level.rgba.end());
        }
    }
    out = std::move(bytes);
    return true;
}

bool TextureStateCodec::Decode(const std::vector<uint8_t>& bytes, TextureAssets::Snapshot& out,
                               std::string* error) {
    if (bytes.size() < 20 || bytes.size() > kMaxPayloadBytes) return Bad(error, "invalid payload extent");
    Reader reader{bytes};
    bool ok = true;
    if (reader.Read(4, ok) != kTag || reader.Read(4, ok) != kVersion)
        return Bad(error, "wrong tag or unsupported version");
    TextureAssets::Snapshot snapshot;
    snapshot.next_generation = reader.Read(8, ok);
    const auto count = reader.Read(4, ok);
    if (!ok || count > TextureAssets::Limits{}.versions || count > (bytes.size() - reader.at) / 36)
        return Bad(error, "invalid asset count");
    snapshot.assets.reserve(size_t(count));
    for (size_t i = 0; i < count; ++i) {
        TextureAssets::SavedAsset asset;
        asset.token.object = uint32_t(reader.Read(4, ok));
        asset.asset_id = uint32_t(reader.Read(4, ok));
        asset.token.generation = reader.Read(8, ok);
        const auto format = reader.Read(1, ok), alpha = reader.Read(1, ok), function = reader.Read(1, ok);
        const auto levels = reader.Read(1, ok), palette = reader.Read(4, ok);
        if (!ok || format > 1 || alpha > 1 || function > 3 || !levels || levels > 7 ||
            palette != (format == 1 ? 256u : 0u) || (!format && levels != 1))
            return Bad(error, "invalid image metadata");
        asset.image.format = TextureCodec::Format(format);
        asset.image.alpha = alpha != 0;
        asset.image.function = uint8_t(function);
        asset.image.palette.resize(size_t(palette));
        for (auto& colour : asset.image.palette)
            if (!reader.Raw(colour.data(), colour.size())) return Bad(error, "truncated palette");
        for (size_t j = 0; j < levels; ++j) {
            TextureCodec::Level level;
            level.width = uint32_t(reader.Read(4, ok)); level.height = uint32_t(reader.Read(4, ok));
            const auto size = reader.Read(4, ok);
            if (!ok || !Power(level.width) || !Power(level.height) ||
                size != uint64_t(level.width) * level.height * 4 || size > bytes.size() - reader.at)
                return Bad(error, "invalid level extent");
            level.rgba.resize(size_t(size));
            if (!reader.Raw(level.rgba.data(), level.rgba.size())) return Bad(error, "truncated level");
            asset.image.levels.push_back(std::move(level));
        }
        asset.image.content_hash = TextureCodec::ContentHash(asset.image);
        snapshot.assets.push_back(std::move(asset));
    }
    if (reader.at != bytes.size()) return Bad(error, "unclassified payload tail");
    if (!TextureAssets::ValidateSnapshot(snapshot, TextureAssets::Limits{}, error)) return false;
    out = std::move(snapshot);
    return true;
}

bool TextureStateCodec::Validate(const std::vector<uint8_t>& bytes, std::string* error) {
    TextureAssets::Snapshot snapshot;
    return Decode(bytes, snapshot, error);
}

}  // namespace title
