#pragma once
#include <map>
#include <memory>
#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.h"

namespace title {
// ModelRead completion converts authored data once; drawing uses this immutable
// allocation. Source object reuse cannot retarget previously queued instances.
class MeshAssets {
public:
    static constexpr uint32_t kModelRead = 0x001bcb58, kModelDestroy = 0x001c0b08;
    static constexpr uint32_t kDiskManager = 0x0031b138;
    struct Model { std::vector<MeshGeometry::Mesh> parts; size_t vertices = 0; };
    using ModelLease = std::shared_ptr<const Model>;
    enum class Result { Ok, InvalidMemory, UnsupportedAsset, LimitExceeded };
    Result ModelLoaded(const MaterialState::Read& read, uint32_t object);
    void ForgetModel(uint32_t object);
    ModelLease FindModel(uint32_t object) const;
    size_t Count() const { return models_.size(); }
    void Reset();
private:
    std::map<uint32_t, ModelLease> models_;
    size_t cached_vertices_ = 0;
};
}  // namespace title
