#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_assets.h"

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
    if (!Value(read, MeshAssets::kDiskManager + 0x58 + uint32_t(handle) * 4, node) ||
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

MeshAssets::Result MeshAssets::ModelLoaded(const Read& read, uint32_t object) {
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
            if (vertices > MeshGeometry::kMaxVertices) return Result::LimitExceeded;
            std::vector<uint8_t> storage;
            if (!Block(read, handle, size, storage)) return Result::InvalidMemory;
            MeshGeometry::Mesh mesh;
            if (MeshGeometry::Decode(storage.data(), storage.size(), declared, mesh) != MeshGeometry::Result::Ok)
                return Result::UnsupportedAsset;
            model->parts.push_back(std::move(mesh));
        }
        if (vertices > 4 * 1024 * 1024 - cached_vertices_) return Result::LimitExceeded;
        model->vertices = vertices;
        models_.emplace(object, std::move(model));
        cached_vertices_ += vertices;
        return Result::Ok;
    } catch (...) { return Result::InvalidMemory; }
}

void MeshAssets::ForgetModel(uint32_t object) {
    const auto found = models_.find(object);
    if (found == models_.end()) return;
    cached_vertices_ -= found->second->vertices;
    models_.erase(found);
}
MeshAssets::ModelLease MeshAssets::FindModel(uint32_t object) const {
    const auto found = models_.find(object);
    return found == models_.end() ? ModelLease{} : found->second;
}
void MeshAssets::Reset() { models_.clear(); cached_vertices_ = 0; }
}  // namespace title
