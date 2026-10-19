#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets.h"

#include <algorithm>
#include <set>

#include "games/slus_20909_crash_twinsanity/hle/workarounds/meta_arena.h"

namespace title {
namespace {
using Read = MaterialState::Read;
uint32_t Physical(uint32_t address) {
    const uint32_t segment = address >> 29;
    return segment == 0 || segment == 4 || segment == 5 ? address & 0x1fffffff : 0;
}
// Readable extent: main RAM plus the META arena right above it -- the title's
// DiskManager nodes live there under hle.meta_arena routing (MetaArena), and
// the node is how a model's loaded bytes are found. The reader decides what it
// actually serves; this only bounds the arithmetic.
constexpr uint32_t kReadableEnd = MetaArena::kBase + MetaArena::kSize;
template<class T> bool Value(const Read& read, uint32_t address, T& out) {
    address = Physical(address);
    return read && address && !(address & 3) && address <= kReadableEnd &&
           sizeof(T) <= kReadableEnd - address && read(address, &out, sizeof(T));
}
bool Source(const Read& read, uint32_t object, std::array<uint32_t, 7>& model,
            uint32_t& vertices, uint32_t& size) {
    return Value(read, object, model) && model[2] == 1 &&
           Value(read, model[3], vertices) && Value(read, model[4], size);
}
bool Bytes(const Read& read, uint32_t handles, uint32_t size, std::vector<uint8_t>& bytes) {
    int32_t handle = -1;
    uint32_t pointer = 0;
    std::array<uint32_t, 8> allocation{};
    if (!size || size > 4 * 1024 * 1024 ||
        !Value(read, handles, handle) || handle < 0 || handle >= 5500 ||
        !Value(read, ShadowAssets::kDiskManager + 0x58 + uint32_t(handle) * 4, pointer) ||
        !Value(read, pointer, allocation) || allocation[0] != uint32_t(handle) ||
        allocation[2] < size || !(allocation[7] & 15) ||
        (!(allocation[7] & (1 << 14)) && ((allocation[7] >> 15) & 3) != 2)) return false;
    const uint32_t address = Physical(allocation[1]);
    if (!address || address > 0x02000000 || size > 0x02000000 - address) return false;
    bytes.resize(size);
    return read(address, bytes.data(), size);
}
}
bool ShadowAssets::IsSourceModel(uint32_t id) {
    constexpr std::array<uint32_t, 8> package{
        0xd8d37a1c, 0x6bb7b765, 0xf37a152c, 0x92c82e8b,
        0xb2da733d, 0x09b44da5, 0x2adc18fb, 0x231cda3b};
    return (id >= 0x67 && id < 0x6f) || std::find(package.begin(), package.end(), id) != package.end();
}
void ShadowAssets::Collect() {
    versions_.erase(std::remove_if(versions_.begin(), versions_.end(),
                                  [](const auto& v) { return v.expired(); }), versions_.end());
}
ShadowAssets::Result ShadowAssets::ModelLoaded(const Read& read, uint32_t object) {
    object = Physical(object);
    Forget(object);
    Collect();
    const auto fail = [&](Result result) { ++counts_.rejected; return result; };
    try {
        std::array<uint32_t, 7> source{};
        if (!Value(read, object, source)) return fail(Result::InvalidMemory);
        if (!IsSourceModel(source[1])) return Result::NotSelected;
        uint32_t vertices = 0, size = 0;
        if (!Source(read, object, source, vertices, size)) return fail(Result::InvalidMemory);
        if (!next_generation_ || current_.size() >= kMaxCurrent || versions_.size() >= kMaxVersions)
            return fail(Result::LimitExceeded);
        auto asset = std::make_shared<Asset>();
        if (!Bytes(read, source[6], size, asset->bytes)) return fail(Result::InvalidMemory);
        const auto decoded = ShadowGeometry::Decode(asset->bytes.data(), asset->bytes.size(),
                                                    vertices, asset->mesh);
        if (decoded != ShadowGeometry::Result::Ok)
            return fail(decoded == ShadowGeometry::Result::LimitExceeded ?
                        Result::LimitExceeded : Result::InvalidAsset);
        asset->token = {object, next_generation_};
        asset->id = source[1];
        asset->vertices = vertices;
        // Allocate both containers before publishing. Failure retains no entry.
        versions_.reserve(versions_.size() + 1);
        current_.emplace(object, asset);
        versions_.push_back(asset);
        ++next_generation_;
        ++counts_.loaded;
        return Result::Ok;
    } catch (...) { return fail(Result::InvalidMemory); }
}
ShadowAssets::Lease ShadowAssets::Find(uint32_t object) const {
    const auto at = current_.find(Physical(object));
    return at == current_.end() ? Lease{} : at->second;
}
void ShadowAssets::Forget(uint32_t object) {
    counts_.released += current_.erase(Physical(object));
}
bool ShadowAssets::HasConsumers() const {
    for (const auto& weak : versions_) {
        const auto asset = weak.lock();
        if (!asset) continue;
        const auto at = current_.find(asset->token.object);
        if (at == current_.end() || at->second != asset || asset.use_count() != 2) return true;
    }
    return false;
}
ShadowAssets::Result ShadowAssets::ResetAfterQuiescence() {
    if (HasConsumers()) return Result::RetainedConsumer;
    current_.clear();
    versions_.clear();
    return Result::Ok;
}
ShadowAssets::Result ShadowAssets::Capture(Snapshot& out) const {
    if (HasConsumers()) return Result::RetainedConsumer;
    Snapshot snapshot;
    snapshot.next_generation = next_generation_;
    for (const auto& pair : current_) {
        const auto& asset = *pair.second;
        snapshot.assets.push_back({asset.token, asset.id, asset.vertices, asset.bytes});
    }
    std::sort(snapshot.assets.begin(), snapshot.assets.end(),
              [](const auto& a, const auto& b) { return a.token.generation < b.token.generation; });
    out = std::move(snapshot);
    return Result::Ok;
}
bool ShadowAssets::Validate(const Snapshot& snapshot) {
    if (snapshot.version != 1 || snapshot.assets.size() > kMaxCurrent) return false;
    uint64_t previous = 0;
    std::set<uint32_t> objects;
    for (const auto& asset : snapshot.assets) {
        const auto& token = asset.token;
        if (!token.object || token.object >= 0x02000000 || (token.object & 3) ||
            token.generation <= previous || !objects.insert(token.object).second ||
            !IsSourceModel(asset.id)) return false;
        ShadowGeometry::Mesh mesh;
        if (ShadowGeometry::Decode(asset.bytes.data(), asset.bytes.size(), asset.vertices, mesh) !=
            ShadowGeometry::Result::Ok) return false;
        previous = token.generation;
    }
    return !snapshot.next_generation || snapshot.next_generation > previous;
}
bool ShadowAssets::ValidateSource(const Snapshot& snapshot, const Read& read) {
    if (!Validate(snapshot)) return false;
    try {
        for (const auto& asset : snapshot.assets) {
            std::array<uint32_t, 7> source{};
            uint32_t vertices = 0, size = 0;
            if (!Source(read, asset.token.object, source, vertices, size) ||
                source[1] != asset.id || vertices != asset.vertices || size != asset.bytes.size())
                return false;
        }
    } catch (...) { return false; }
    return true;
}
bool ShadowAssets::ValidateConsumers(const Snapshot& snapshot, const Read& read) {
    try {
        std::array<uint32_t, 8> shapes{};
        if (!Value(read, kMeshes, shapes)) return false;
        for (uint32_t object : shapes) {
            if (!object) continue; // source development names may omit a shape
            std::array<uint32_t, 8> mesh{};
            // No open instance block, no billboard (the type-0B shader answers
            // NeedsEye, never Billboard), one material slot.
            if (!Value(read, object, mesh) || mesh[4] || (mesh[5] & 0xff00) || mesh[6] != 1)
                return false;
            const uint32_t geometry = Physical(mesh[3]);
            const auto asset = std::find_if(snapshot.assets.begin(), snapshot.assets.end(),
                [&](const auto& item) { return item.token.object == geometry && item.id == mesh[7]; });
            if (asset == snapshot.assets.end()) return false;
            uint32_t resource = 0, material_at = 0, shader_type = 0, vtable = 0;
            std::array<uint32_t, 11> material{};
            if (!Value(read, mesh[2], resource) || resource > 0xfffffff3 ||
                !Value(read, resource + 8, material_at) || !Value(read, material_at, material) ||
                material[4] != 1 || material[6] != 32 || material[7] ||
                material[8] != 21 || material[10] || material[0] > 0xffffff90 ||
                !Value(read, material[0] + 0x64, shader_type) ||
                !Value(read, material[0] + 0x6c, vtable) || shader_type != 11 || vtable != kShaderVtable)
                return false;
        }
    } catch (...) { return false; }
    return true;
}
ShadowAssets::Result ShadowAssets::Restore(const Snapshot& snapshot) {
    if (!current_.empty() || next_generation_ != 1 || HasConsumers()) return Result::OwnerNotEmpty;
    if (!Validate(snapshot)) return Result::InvalidSnapshot;
    std::map<uint32_t, Lease> staged;
    std::vector<std::weak_ptr<const Asset>> versions;
    try {
        for (const auto& saved : snapshot.assets) {
            auto asset = std::make_shared<Asset>();
            asset->token = saved.token;
            asset->id = saved.id;
            asset->vertices = saved.vertices;
            asset->bytes = saved.bytes;
            if (ShadowGeometry::Decode(asset->bytes.data(), asset->bytes.size(),
                                      asset->vertices, asset->mesh) != ShadowGeometry::Result::Ok)
                return Result::InvalidSnapshot;
            staged.emplace(asset->token.object, asset);
            versions.push_back(asset);
        }
    } catch (...) { return Result::LimitExceeded; }
    current_.swap(staged);
    versions_.swap(versions);
    next_generation_ = snapshot.next_generation;
    return Result::Ok;
}
}  // namespace title
