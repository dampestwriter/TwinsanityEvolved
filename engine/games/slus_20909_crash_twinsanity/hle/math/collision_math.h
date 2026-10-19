#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

// Source collision operations. Value APIs own their pending edge tests and
// grouped support points. Register/memory adapters serve unmigrated callers;
// no VU instruction, program image, dispatch cycle or interpreter is executed.
class CollisionMath {
public:
    using Vector = MathLibrary::Vector;
    using Matrix = MathLibrary::Matrix;
    using GuestMemory = MathLibrary::GuestMemory;
    using Body = MathLibrary::Body;
    using Capture = MathLibrary::Capture;
    using Installer = MathLibrary::Installer;
    using Triangle = std::array<Vector, 3>;
    using PointGroup = std::array<Vector, 3>;  // Four x, four y, four z values.
    using HullSupport = std::array<PointGroup, 6>;  // 3 triangle x 8 hull points.

    struct EdgeSetup {
        Vector direction;
        std::array<Vector, 3> edges;
        Vector diagonal;
        Vector normal;
        std::array<Vector, 3> edge_normals;
        // First pair already multiplied, remaining four await the plane test.
        std::array<Vector, 6> pending;
        std::array<Vector, 2> plane_products;
        std::array<float, 2> plane_values;
        Vector last_cross_products;
    };

    struct EdgeValues {
        std::array<Vector, 6> products;
        std::array<float, 6> values;
    };

    struct Projection {
        Vector values;
        Vector partial;
    };

    struct SupportProjection {
        std::array<float, 2> range;
        Vector minimum;
        Vector maximum;
        Vector lower_three;
        Vector upper_four;
        Vector upper_five;
        Vector penultimate;
        Vector partial;
    };

    struct Counters {
        std::array<uint64_t, 8> entries;
        uint64_t generated_continuations;
        uint64_t unsupported_inputs;
        // Same operation order as entries; actual captured-body executions,
        // including control selection and every delegated execution.
        std::array<uint64_t, 8> generated_body_calls;
    };

    static constexpr uint32_t kStartEdgesAddress = 0x00292ae8;
    static constexpr uint32_t kFinishEdgesAddress = 0x00292b8c;
    static constexpr uint32_t kVertexDifferencesAddress = 0x00292c04;
    static constexpr uint32_t kGroupPointsAddress = 0x00292ce8;
    static constexpr uint32_t kGroupsRangeAddress = 0x00292d44;
    static constexpr uint32_t kSixteenGroupsRangeAddress = 0x00292dcc;
    static constexpr uint32_t kLoadSupportAddress = 0x00292e80;
    static constexpr uint32_t kSupportRangeAddress = 0x00293064;

    explicit CollisionMath(GuestMemory& memory);
    ~CollisionMath();
    // Separate opt-in batch. Interiors and nonpositive loop counts retain the
    // original body and are counted; neither is a claimed retired execution.
    // Same all-or-none activation and compensating rollback as MathLibrary.
    // false instruments the eight pristine generated originals; true replaces
    // supported full entries. Capture must not return installed wrappers.
    // One successful batch per owner; teardown requires quiescent dispatch.
    // Retained callbacks forward to originals after destruction.
    bool Install(const Capture& capture, const Installer& installer, bool replace_helpers = true);

    static EdgeSetup EvaluateStartEdges(const Vector& start, const Vector& end, const Triangle& triangle);
    static EdgeValues EvaluateFinishEdges(const EdgeSetup& pending);
    static PointGroup PackPoints(const std::array<Vector, 4>& points);
    static Projection ProjectGroup(const PointGroup& group, const Vector& axis);
    static HullSupport BuildHullSupport(const Triangle& triangle, const std::array<Vector, 8>& hull);
    static SupportProjection ProjectHullSupport(const HullSupport& support, const Vector& axis);

    // Complete NTSC adapters. Source provenance: collisionmaths.cpp 10, 57,
    // 95, 165, 199, 244, 301, 367; the current generated shard 027 is the ABI.
    void StartTriangleEdgeTests(CpuContext& context);
    void FinishTriangleEdgeTests(CpuContext& context);
    // Counts must be positive; prefetch-before-store order and repeated
    // request reads are retained, including overlapping moved/difference data.
    void MakeVertexDifferences(CpuContext& context);
    void GroupPoints(CpuContext& context);
    void GroupsRange(CpuContext& context);
    void SixteenGroupsRange(CpuContext& context);
    void LoadTriangleHullSupport(CpuContext& context);
    void TriangleHullSupportRange(CpuContext& context);

    Counters Counts() const;
    std::string Census() const;

private:
    bool CanReplace(uint32_t address, const CpuContext& context);
    void ProjectMemoryGroups(CpuContext& context, bool sixteen);
    GuestMemory& memory_;
    std::array<std::atomic<uint64_t>, 8> calls_{};
    std::shared_ptr<std::atomic<uint64_t>> continuations_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> installation_;
    std::atomic<uint64_t> unsupported_{0};
    std::shared_ptr<std::array<std::atomic<uint64_t>, 8>> generated_body_calls_;
};

}  // namespace title
