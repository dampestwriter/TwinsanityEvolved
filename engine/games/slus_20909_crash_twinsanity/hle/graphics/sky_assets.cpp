#include "games/slus_20909_crash_twinsanity/hle/graphics/sky_assets.h"

#include <array>

namespace title {
namespace {
using Read = MaterialState::Read;
template<class T> bool Value(const Read& read, uint32_t address, T& out) {
    return read && address && uint64_t(address) + sizeof(T) <= uint64_t{1} << 32 &&
           read(address, &out, sizeof(T));
}
bool Block(const Read& read, int32_t handle, uint32_t size, std::vector<uint8_t>& out) {
    // GetDiskManager/DiskLoadedMemory's SOFTWARE allocation table. No guest
    // call, device access, pointer retention, or live packet interpretation.
    if (handle < 0 || handle >= 5500 || !size || size > 4 * 1024 * 1024) return false;
    uint32_t node = 0;
    std::array<uint32_t, 8> allocation{};
    if (!Value(read, SkyAssets::kDiskManager + 0x58 + uint32_t(handle) * 4, node) ||
        !Value(read, node, allocation) || allocation[0] != uint32_t(handle) ||
        !allocation[1] || allocation[2] < size ||
        uint64_t(allocation[1]) + size > uint64_t{1} << 32 ||
        !(allocation[7] & 15) ||
        (!(allocation[7] & (1 << 14)) && ((allocation[7] >> 15) & 3) != 2))
        return false;
    out.resize(size);
    return read(allocation[1], out.data(), out.size());
}
}  // namespace

SkyAssets::Result SkyAssets::ModelLoaded(const Read& read, uint32_t object) {
    ForgetModel(object);
    if (models_.size() >= 4096) return Result::LimitExceeded;
    try {
        std::array<uint32_t, 7> source{};
        if (!Value(read, object, source)) return Result::InvalidMemory;
        const uint32_t count = source[2];
        if (!count || count > 64) return Result::UnsupportedAsset;
        auto model = std::make_shared<Model>();
        size_t vertices = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t declared = 0, size = 0;
            int32_t handle = -1;
            if (source[3] > 0xffffff00 || source[4] > 0xffffff00 || source[6] > 0xffffff00 ||
                !Value(read, source[3] + i * 4, declared) ||
                !Value(read, source[4] + i * 4, size) ||
                !Value(read, source[6] + i * 4, handle)) return Result::InvalidMemory;
            vertices += declared;
            if (vertices > SkyGeometry::kMaxVertices) return Result::LimitExceeded;
            std::vector<uint8_t> storage;
            if (!Block(read, handle, size, storage)) return Result::InvalidMemory;
            SkyGeometry::Mesh mesh;
            if (SkyGeometry::Decode(storage.data(), storage.size(), declared, mesh) != SkyGeometry::Result::Ok)
                return Result::UnsupportedAsset;
            model->parts.push_back(std::move(mesh));
        }
        models_.emplace(object, ModelEntry{source[1], std::move(model)});
        return Result::Ok;
    } catch (...) { return Result::InvalidMemory; }
}

SkyAssets::Result SkyAssets::TextureLoaded(const Read& read, uint32_t object, const Converted& converted) {
    ForgetTexture(object);
    if (textures_.size() >= 4096) return Result::LimitExceeded;
    try {
        std::array<uint32_t, 27> source{};
        if (!Value(read, object, source)) return Result::InvalidMemory;
        if (source[2] <= 128) return Result::UnsupportedAsset;
        const auto* header = reinterpret_cast<const uint8_t*>(source.data()) + 12;
        auto texture = std::make_shared<SkyTexture::Image>();
        if (source[24] == 0xffffffff) {
            // The texture owner's ownership word: no transfer block exists for
            // this object; its pixels are the owned converted asset.
            const auto image = converted ? converted(object) : nullptr;
            if (!image) return Result::NotConverted;
            if (SkyTexture::FromConverted(header, 96, *image, *texture) != SkyTexture::Result::Ok)
                return Result::UnsupportedAsset;
        } else {
            std::vector<uint8_t> storage;
            if (!Block(read, int32_t(source[24]), source[2] - 128, storage)) return Result::InvalidMemory;
            if (SkyTexture::Decode(header, 96, storage.data(), storage.size(), *texture) != SkyTexture::Result::Ok)
                return Result::UnsupportedAsset;
        }
        textures_.emplace(object, TextureEntry{source[1], std::move(texture)});
        return Result::Ok;
    } catch (...) { return Result::InvalidMemory; }
}
void SkyAssets::ForgetModel(uint32_t object) { models_.erase(object); }
void SkyAssets::ForgetTexture(uint32_t object) { textures_.erase(object); }
SkyAssets::ModelLease SkyAssets::FindModel(uint32_t object) const {
    const auto found = models_.find(object);
    return found == models_.end() ? ModelLease{} : found->second.lease;
}
SkyAssets::TextureLease SkyAssets::FindTexture(uint32_t object) const {
    const auto found = textures_.find(object);
    return found == textures_.end() ? TextureLease{} : found->second.lease;
}
void SkyAssets::Reset() { models_.clear(); textures_.clear(); }
}  // namespace title
