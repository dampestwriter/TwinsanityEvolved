#pragma once

#include <optional>
#include <string>
#include <variant>

#include "games/slus_20909_crash_twinsanity/hle/math/culling_math.h"

namespace title {

// The renderer's active culling view and pending source result. No transport
// addresses or vector-unit registers are needed by these consumers.
class CullingState {
public:
    using Result = std::variant<std::monostate, CullingMath::Classification,
                                CullingMath::Model, CullingMath::BoxView>;
    struct Snapshot {
        uint32_t version = 1;
        std::optional<CullingMath::View> view;
        std::optional<CullingMath::Link> link;
        std::optional<CullingMath::Frustum> planes;
        Result result;
    };

    // Loading a view resets its link to identity; a linked chunk then supplies
    // chunk/object matrices. Plane loading does not change either view or link.
    void LoadView(const CullingMath::View& view);
    void LoadPlanes(const CullingMath::Frustum& planes);
    bool LoadLink(const CullingMath::Matrix& chunk, const CullingMath::Matrix& object);
    bool LoadModel(const CullingMath::Matrix& model);
    bool TestCell(const CullingMath::Box& box);
    bool TestBoxAt(const CullingMath::Box& box, const CullingMath::Matrix& model);

    // Readers retain their result until another producer runs. Returning by
    // value lets a caller start the next item's test before drawing this one,
    // as DrawCellInstances and SceneryMeshes do in the source.
    std::optional<CullingMath::Classification> CellResult() const;
    std::optional<CullingMath::Model> DrawnModelResult() const;
    std::optional<CullingMath::BoxView> MeshResult() const;
    std::optional<CullingMath::BoxView> InstanceResult() const;

    void Reset();
    Snapshot Capture() const;
    static bool Validate(const Snapshot& snapshot, std::string* error = nullptr);
    // Failure leaves all live state unchanged. No boot-era state is retained.
    bool Restore(const Snapshot& snapshot, std::string* error = nullptr);

private:
    Snapshot state_;
};

}  // namespace title
