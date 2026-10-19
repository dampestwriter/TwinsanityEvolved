#include "games/slus_20909_crash_twinsanity/hle/math/culling_state_codec.h"

#include <cstring>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/input/snapshot_bytes.h"

namespace title {
namespace {
bool Vector(SnapshotBytes& io, CullingMath::Vector& vector) {
    for (auto& lane : vector) {
        uint32_t word;
        static_assert(sizeof(lane) == sizeof(word));
        std::memcpy(&word, &lane, sizeof(word));
        if (!io.U32(word)) return false;
        std::memcpy(&lane, &word, sizeof(word));
    }
    return true;
}
bool Matrix(SnapshotBytes& io, CullingMath::Matrix& matrix) {
    for (auto& row : matrix) if (!Vector(io, row)) return false;
    return true;
}
template<class T> bool View(SnapshotBytes& io, T& view) {
    return Matrix(io, view.to_clip) && Matrix(io, view.to_screen) && Vector(io, view.camera);
}
bool Columns(SnapshotBytes& io, CullingMath::PlaneColumns& columns) {
    for (auto& row : columns.absolute_normals) if (!Vector(io, row)) return false;
    for (auto& row : columns.normals) if (!Vector(io, row)) return false;
    return Vector(io, columns.offsets);
}
bool Planes(SnapshotBytes& io, CullingMath::Frustum& planes) {
    return Columns(io, planes.sides) && Columns(io, planes.depth) && Columns(io, planes.far_sides);
}
bool Visibility(SnapshotBytes& io, CullingMath::Classification& value) {
    return io.U32(value.outside) && io.U32(value.clipped);
}
template<class T, class Fields>
bool Optional(SnapshotBytes& io, std::optional<T>& value, const Fields& fields) {
    bool present = value.has_value();
    if (!io.Boolean(present)) return false;
    if (!present) { value.reset(); return true; }
    if (!value) value.emplace(T{});
    return fields(io, *value);
}
bool Fields(SnapshotBytes& io, CullingState::Snapshot& snapshot) {
    uint32_t tag = CullingStateCodec::kTag;
    if (!io.U32(tag) || tag != CullingStateCodec::kTag ||
        !io.U32(snapshot.version) || snapshot.version != 1 ||
        !Optional(io, snapshot.view, View<CullingMath::View>) ||
        !Optional(io, snapshot.link, View<CullingMath::Link>) ||
        !Optional(io, snapshot.planes, Planes)) return false;
    uint8_t kind = static_cast<uint8_t>(snapshot.result.index());
    if (!io.U8(kind)) return false;
    switch (kind) {
        case 0: snapshot.result = std::monostate{}; break;
        case 1: {
            auto value = std::get_if<CullingMath::Classification>(&snapshot.result);
            CullingMath::Classification fields = value ? *value : CullingMath::Classification{};
            if (!Visibility(io, fields)) return false;
            snapshot.result = fields;
            break;
        }
        case 2: {
            auto value = std::get_if<CullingMath::Model>(&snapshot.result);
            CullingMath::Model fields = value ? *value : CullingMath::Model{};
            if (!Matrix(io, fields.to_screen) || !io.I32(fields.distance)) return false;
            snapshot.result = fields;
            break;
        }
        case 3: {
            auto value = std::get_if<CullingMath::BoxView>(&snapshot.result);
            CullingMath::BoxView fields = value ? *value : CullingMath::BoxView{};
            if (!Visibility(io, fields.visibility) || !io.I32(fields.distance) ||
                !Optional(io, fields.to_screen, Matrix) || !Optional(io, fields.to_clip, Matrix)) return false;
            snapshot.result = fields;
            break;
        }
        default: return false;
    }
    return io.Done();
}
bool Invalid(std::string* error) {
    if (error) *error = "Culling: invalid snapshot length, version or encoding";
    return false;
}
}

bool CullingStateCodec::Encode(const CullingState::Snapshot& snapshot,
        std::vector<uint8_t>& output, std::string* error) {
    if (!CullingState::Validate(snapshot, error)) return false;
    auto copy = snapshot;
    std::vector<uint8_t> bytes;
    SnapshotBytes io(bytes, kMaxBytes);
    if (!Fields(io, copy)) return Invalid(error);
    output = std::move(bytes);
    return true;
}

bool CullingStateCodec::Decode(const std::vector<uint8_t>& bytes,
        CullingState::Snapshot& output, std::string* error) {
    if (bytes.size() > kMaxBytes) return Invalid(error);
    CullingState::Snapshot snapshot;
    SnapshotBytes io(bytes);
    if (!Fields(io, snapshot)) return Invalid(error);
    if (!CullingState::Validate(snapshot, error)) return false;
    output = std::move(snapshot);
    return true;
}

bool CullingStateCodec::Validate(const std::vector<uint8_t>& bytes, std::string* error) {
    CullingState::Snapshot snapshot;
    return Decode(bytes, snapshot, error);
}

}  // namespace title
