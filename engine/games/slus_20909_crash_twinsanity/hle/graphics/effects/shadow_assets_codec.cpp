#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets_codec.h"

namespace title {
namespace {
void Put(std::vector<uint8_t>& out, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) out.push_back(uint8_t(value >> (i * 8)));
}
}
bool ShadowAssetsCodec::Encode(const ShadowAssets::Snapshot& snapshot, std::vector<uint8_t>& out) {
    if (!ShadowAssets::Validate(snapshot)) return false;
    size_t size = 20;
    for (const auto& asset : snapshot.assets) size += 24 + asset.bytes.size();
    if (size > kMaxBytes) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(size);
    Put(bytes, kMagic, 4); Put(bytes, snapshot.version, 4);
    Put(bytes, snapshot.next_generation, 8); Put(bytes, snapshot.assets.size(), 4);
    for (const auto& asset : snapshot.assets) {
        Put(bytes, asset.token.object, 4); Put(bytes, asset.id, 4);
        Put(bytes, asset.token.generation, 8); Put(bytes, asset.vertices, 4);
        Put(bytes, asset.bytes.size(), 4);
        bytes.insert(bytes.end(), asset.bytes.begin(), asset.bytes.end());
    }
    out = std::move(bytes);
    return true;
}
bool ShadowAssetsCodec::Decode(const std::vector<uint8_t>& bytes, ShadowAssets::Snapshot& out) {
    if (bytes.size() < 20 || bytes.size() > kMaxBytes) return false;
    size_t at = 0;
    bool ok = true;
    const auto read = [&](unsigned size) {
        if (size > bytes.size() - at) { ok = false; return uint64_t{0}; }
        uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i) result |= uint64_t(bytes[at++]) << (i * 8);
        return result;
    };
    if (read(4) != kMagic || read(4) != 1) return false;
    ShadowAssets::Snapshot snapshot;
    snapshot.next_generation = read(8);
    const size_t count = size_t(read(4));
    if (!ok || count > ShadowAssets::kMaxCurrent || count > (bytes.size() - at) / 40) return false;
    for (size_t i = 0; i < count; ++i) {
        ShadowAssets::Saved asset;
        asset.token.object = uint32_t(read(4));
        asset.id = uint32_t(read(4));
        asset.token.generation = read(8);
        asset.vertices = uint32_t(read(4));
        const size_t size = size_t(read(4));
        if (!ok || size > 4 * 1024 * 1024 || size > bytes.size() - at) return false;
        asset.bytes.assign(bytes.begin() + at, bytes.begin() + at + size);
        at += size;
        snapshot.assets.push_back(std::move(asset));
    }
    if (!ok || at != bytes.size() || !ShadowAssets::Validate(snapshot)) return false;
    out = std::move(snapshot);
    return true;
}
}  // namespace title
