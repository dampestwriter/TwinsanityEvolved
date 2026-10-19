#include "games/slus_20909_crash_twinsanity/hle/math/collision_math.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "engine/ee/cpu/ps2_float.h"
#include "engine/ipu/hle/guest_memory.h"

namespace title {
namespace {

using Vector = CollisionMath::Vector;
using PointGroup = CollisionMath::PointGroup;

float Product(float a, float b) {
    volatile float value = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(value);
}

float Sum(float a, float b) {
    volatile float value = ps2_float_clamp(a) + ps2_float_clamp(b);
    return ps2_float_clamp(value);
}

float Difference(float a, float b) {
    volatile float value = ps2_float_clamp(a) - ps2_float_clamp(b);
    return ps2_float_clamp(value);
}

Vector SubtractPoints(const Vector& a, const Vector& b) {
    return {Difference(a[0], b[0]), Difference(a[1], b[1]), Difference(a[2], b[2]), 0};
}

Vector MultiplyPoints(const Vector& a, const Vector& b) {
    return {Product(a[0], b[0]), Product(a[1], b[1]), Product(a[2], b[2]), 0};
}

// Cross terms at these source boundaries are raw scalar products; unlike
// the following dot products, they do not pass through the vector clamp.
Vector CrossTerms(const Vector& a, const Vector& b) {
    Vector value{};
    for (size_t lane = 0; lane < 3; ++lane) {
        volatile float term = a[(lane + 1) % 3] * b[(lane + 2) % 3];
        value[lane] = term;
    }
    return value;
}

Vector Cross(const Vector& a, const Vector& b) {
    const auto first = CrossTerms(a, b), second = CrossTerms(b, a);
    Vector value{};
    for (size_t lane = 0; lane < 3; ++lane) {
        volatile float term = first[lane] - second[lane];
        value[lane] = term;
    }
    return value;
}

float DotSum(const Vector& products) {
    return Sum(Sum(products[0], products[1]), products[2]);
}

// Compare directly and copy the selected word. No arithmetic clamp, denormal
// flushing, epsilon equality or alternate signed-zero ordering at reductions.
Vector Minimum(const Vector& a, const Vector& b) {
    Vector value;
    for (size_t i = 0; i < 4; ++i) value[i] = std::min(a[i], b[i]);
    return value;
}

Vector Maximum(const Vector& a, const Vector& b) {
    Vector value;
    for (size_t i = 0; i < 4; ++i) value[i] = std::max(a[i], b[i]);
    return value;
}

Vector Read(CollisionMath::GuestMemory& memory, uint32_t address) {
    Vector value;
    address &= ~15u;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        const auto bits = memory.Read32(address + lane * 4);
        std::memcpy(&value[lane], &bits, sizeof(bits));
    }
    return value;
}

void Write(CollisionMath::GuestMemory& memory, uint32_t address, const Vector& value) {
    address &= ~15u;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        uint32_t bits;
        std::memcpy(&bits, &value[lane], sizeof(bits));
        memory.Write32(address + lane * 4, bits);
    }
}

Vector Value(const VR_reg& reg) {
    Vector value;
    std::memcpy(value.data(), reg.F, 16);
    return value;
}

void Publish(VR_reg& reg, const Vector& value) {
    std::memcpy(reg.F, value.data(), 16);
}

void PublishPoint(VR_reg& reg, const Vector& value) {
    std::memcpy(reg.F, value.data(), 12);
}

void Publish(GPR_reg& reg, const Vector& value) {
    std::memcpy(reg.UL, value.data(), 16);
}

void Advance(GPR_reg& reg, uint32_t bytes) {
    reg.SD[0] = static_cast<int32_t>(reg.UL[0] + bytes);
}

void PublishPacked(CpuContext& context, const std::array<Vector, 4>& points) {
    // Raw packed-word scratch values, independent of how host point packing
    // is evaluated. The fourth original point remains in r13.
    auto& r = context.cpuRegs.GPR.r;
    const auto packed = CollisionMath::PackPoints(points);
    for (size_t i = 0; i < 3; ++i) Publish(r[10 + i], packed[i]);
    Publish(r[13], points[3]);
    Publish(r[14], {points[0][0], points[1][0], points[0][1], points[1][1]});
    Publish(r[15], {points[0][2], points[1][2], points[0][3], points[1][3]});
    Publish(r[24], {points[2][2], points[3][2], points[2][3], points[3][3]});
}

PointGroup ReadGroup(CollisionMath::GuestMemory& memory, uint32_t address) {
    return {Read(memory, address), Read(memory, address + 16), Read(memory, address + 32)};
}

void PublishRange(CpuContext& context, const Vector& minimum, const Vector& maximum) {
    auto& v = context.vuRegs.VF;
    v[8].x = std::min(minimum[0], minimum[1]);
    v[9].z = std::min(minimum[2], minimum[3]);
    v[10].x = std::min(v[8].x, v[9].z);
    v[11].x = std::max(maximum[0], maximum[1]);
    v[12].z = std::max(maximum[2], maximum[3]);
    v[13].x = std::max(v[11].x, v[12].z);
    Publish(context.cpuRegs.GPR.r[8], Value(v[10]));
    Publish(context.cpuRegs.GPR.r[9], Value(v[13]));
}

}  // namespace

CollisionMath::CollisionMath(GuestMemory& memory)
    : memory_(memory), generated_body_calls_(std::make_shared<std::array<std::atomic<uint64_t>, 8>>()) {}

bool CollisionMath::CanReplace(uint32_t address, const CpuContext& context) {
    // Counted loops assume the source ABI's zero register. Invalid inputs are
    // delegated, never turned into an invented empty result or capped loop.
    const auto& r = context.cpuRegs.GPR.r;
    if (r[0].UD[0] != 0) return false;
    if (address == kGroupPointsAddress || address == kGroupsRangeAddress) return r[5].SL[0] > 0;
    if (address == kVertexDifferencesAddress) {
        return static_cast<int32_t>(memory_.Read32(r[4].UL[0] + 8)) > 0 &&
            static_cast<int32_t>(memory_.Read32(r[4].UL[0] + 20)) > 0;
    }
    return true;
}


CollisionMath::EdgeSetup CollisionMath::EvaluateStartEdges(
    const Vector& start, const Vector& end, const Triangle& triangle) {
    EdgeSetup result{};
    result.direction = SubtractPoints(end, start);
    result.diagonal = SubtractPoints(triangle[2], triangle[0]);
    for (size_t edge = 0; edge < 3; ++edge) {
        result.edges[edge] = SubtractPoints(triangle[(edge + 1) % 3], triangle[edge]);
        result.edge_normals[edge] = Cross(result.direction, result.edges[edge]);
    }
    result.normal = Cross(result.edges[0], result.diagonal);
    result.plane_products[0] = MultiplyPoints(SubtractPoints(start, triangle[0]), result.normal);
    result.plane_products[1] = MultiplyPoints(SubtractPoints(end, triangle[0]), result.normal);
    for (size_t i = 0; i < 2; ++i) result.plane_values[i] = DotSum(result.plane_products[i]);
    constexpr size_t vertices[] = {0, 2, 1, 0, 2, 1};
    for (size_t i = 0; i < 6; ++i) result.pending[i] = SubtractPoints(triangle[vertices[i]], start);
    for (size_t i = 0; i < 2; ++i) result.pending[i] = MultiplyPoints(result.pending[i], result.edge_normals[0]);
    result.last_cross_products = CrossTerms(result.direction, result.edges[2]);
    return result;
}

CollisionMath::EdgeValues CollisionMath::EvaluateFinishEdges(const EdgeSetup& pending) {
    EdgeValues result{pending.pending, {}};
    for (size_t i = 2; i < 6; ++i)
        result.products[i] = MultiplyPoints(pending.pending[i], pending.edge_normals[i / 2]);
    for (size_t i = 0; i < 6; ++i) result.values[i] = DotSum(result.products[i]);
    return result;
}

CollisionMath::PointGroup CollisionMath::PackPoints(const std::array<Vector, 4>& points) {
    PointGroup result;
    for (size_t component = 0; component < 3; ++component)
        for (size_t point = 0; point < 4; ++point)
            std::memcpy(&result[component][point], &points[point][component], sizeof(float));
    return result;
}

CollisionMath::Projection CollisionMath::ProjectGroup(const PointGroup& group, const Vector& axis) {
    Projection result;
    for (size_t lane = 0; lane < 4; ++lane) {
        result.partial[lane] = Sum(Product(group[0][lane], axis[0]), Product(group[1][lane], axis[1]));
        result.values[lane] = Sum(result.partial[lane], Product(group[2][lane], axis[2]));
    }
    return result;
}

CollisionMath::HullSupport CollisionMath::BuildHullSupport(
    const Triangle& triangle, const std::array<Vector, 8>& hull) {
    HullSupport result;
    for (size_t group = 0; group < 6; ++group) {
        std::array<Vector, 4> points;
        for (size_t point = 0; point < 4; ++point)
            points[point] = SubtractPoints(triangle[group / 2], hull[(group % 2) * 4 + point]);
        result[group] = PackPoints(points);
    }
    return result;
}

CollisionMath::SupportProjection CollisionMath::ProjectHullSupport(
    const HullSupport& support, const Vector& axis) {
    std::array<Projection, 6> projections;
    for (size_t i = 0; i < 6; ++i) projections[i] = ProjectGroup(support[i], axis);
    Vector minimum = Minimum(projections[0].values, projections[1].values);
    Vector maximum = Maximum(projections[0].values, projections[1].values);
    SupportProjection result{};
    minimum = Minimum(minimum, projections[2].values);
    maximum = Maximum(maximum, projections[2].values);
    result.lower_three = minimum;
    minimum = Minimum(minimum, projections[3].values);
    maximum = Maximum(maximum, projections[3].values);
    result.upper_four = maximum;
    minimum = Minimum(minimum, projections[4].values);
    maximum = Maximum(maximum, projections[4].values);
    result.upper_five = maximum;
    result.minimum = Minimum(minimum, projections[5].values);
    result.maximum = Maximum(maximum, projections[5].values);
    result.penultimate = projections[4].values;
    result.partial = projections[5].partial;
    result.range = {std::min(std::min(result.minimum[0], result.minimum[1]),
                             std::min(result.minimum[2], result.minimum[3])),
                    std::max(std::max(result.maximum[0], result.maximum[1]),
                             std::max(result.maximum[2], result.maximum[3]))};
    return result;
}

void CollisionMath::StartTriangleEdgeTests(CpuContext& context) {
    calls_[0].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& v = context.vuRegs.VF;
    const auto start = Read(memory_, r[4].UL[0]), end = Read(memory_, r[5].UL[0]);
    const Triangle triangle{Read(memory_, r[6].UL[0]), Read(memory_, r[6].UL[0] + 16),
                            Read(memory_, r[6].UL[0] + 32)};
    const auto result = EvaluateStartEdges(start, end, triangle);
    Publish(v[4], start);
    Publish(v[5], end);
    for (size_t i = 0; i < 3; ++i) Publish(v[1 + i], triangle[i]);
    PublishPoint(v[7], result.direction);
    PublishPoint(v[10], result.edges[0]);
    PublishPoint(v[11], result.normal);
    for (size_t i = 0; i < 3; ++i) {
        PublishPoint(v[15 + i * 5], result.edges[i]);
        PublishPoint(v[16 + i * 5], result.edge_normals[i]);
        PublishPoint(v[17 + i * 5], result.pending[i * 2]);
        PublishPoint(v[18 + i * 5], result.pending[i * 2 + 1]);
    }
    PublishPoint(context.vuRegs.ACC, result.last_cross_products);
    for (size_t i = 0; i < 2; ++i) {
        PublishPoint(v[12 + i], result.plane_products[i]);
        v[12 + i].x = result.plane_values[i];
        Publish(r[8 + i], Value(v[12 + i]));
        memory_.Write32(r[7].UL[0] + static_cast<uint32_t>(i * 4), r[8 + i].UL[0]);
    }
}

void CollisionMath::FinishTriangleEdgeTests(CpuContext& context) {
    calls_[1].fetch_add(1, std::memory_order_relaxed);
    auto& v = context.vuRegs.VF;
    auto& r = context.cpuRegs.GPR.r;
    EdgeSetup pending{};
    for (size_t i = 0; i < 3; ++i) {
        pending.edge_normals[i] = Value(v[16 + i * 5]);
        pending.pending[i * 2] = Value(v[17 + i * 5]);
        pending.pending[i * 2 + 1] = Value(v[18 + i * 5]);
    }
    const auto result = EvaluateFinishEdges(pending);
    for (size_t i = 0; i < 6; ++i) {
        const size_t index = 17 + (i / 2) * 5 + i % 2;
        PublishPoint(v[index], result.products[i]);
        v[index].x = result.values[i];
        Publish(r[8 + i], Value(v[index]));
    }
    for (uint32_t i = 0; i < 6; ++i) memory_.Write32(r[4].UL[0] + i * 4, r[8 + i].UL[0]);
}

void CollisionMath::MakeVertexDifferences(CpuContext& context) {
    calls_[2].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& v = context.vuRegs.VF;
    const uint32_t request = r[4].UL[0];
    for (uint32_t i = 0; i < 8; ++i)
        r[8 + i].SD[0] = static_cast<int32_t>(memory_.Read32(request + i * 4));
    Matrix matrices[2];
    for (size_t matrix = 0; matrix < 2; ++matrix) {
        for (uint32_t row = 0; row < 4; ++row) {
            matrices[matrix][row] = Read(memory_, r[8 + matrix * 3].UL[0] + row * 16);
            Publish(v[1 + matrix * 4 + row], matrices[matrix][row]);
        }
    }
    for (size_t matrix = 0; matrix < 2; ++matrix) {
        const size_t vertices = matrix == 0 ? 9 : 12, count = vertices + 1;
        auto point = Read(memory_, r[vertices].UL[0]);
        Publish(v[10], point);
        if (matrix == 1) r[8].UD[0] = r[15].UD[0] + r[0].UD[0];
        do {
            const auto transformed = MathLibrary::EvaluateTransformVector(matrices[matrix], point);
            Publish(context.vuRegs.ACC, transformed.accumulator);
            Publish(v[11], transformed.output);
            Advance(r[count], uint32_t(-1));
            Advance(r[15], 16);
            // The source intentionally reads the next vertex, including the
            // one-past element, BEFORE storing the current moved point.
            point = Read(memory_, r[vertices].UL[0] + 16);
            Publish(v[10], point);
            Write(memory_, r[15].UL[0] - 16, transformed.output);
            Advance(r[vertices], 16);
        } while (r[count].SD[0] != 0);
    }
    r[9].SD[0] = static_cast<int32_t>(memory_.Read32(request + 28));
    r[10].SD[0] = static_cast<int32_t>(memory_.Read32(request + 8));
    do {
        const auto first = Read(memory_, r[9].UL[0]);
        Publish(v[1], first);
        r[12].UD[0] = r[8].UD[0] + r[0].UD[0];
        r[13].SD[0] = static_cast<int32_t>(memory_.Read32(request + 20));
        do {
            const auto second = Read(memory_, r[12].UL[0]);
            Publish(v[2], second);
            const auto difference = SubtractPoints(first, second);
            PublishPoint(v[3], difference);
            Advance(r[13], uint32_t(-1));
            Advance(r[12], 16);
            Write(memory_, r[14].UL[0], Value(v[3]));
            Advance(r[14], 16);
        } while (r[13].SD[0] != 0);
        Advance(r[10], uint32_t(-1));
        Advance(r[9], 16);
    } while (r[10].SD[0] != 0);
}

void CollisionMath::GroupPoints(CpuContext& context) {
    calls_[3].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    r[8].UD[0] = r[4].UD[0] + r[0].UD[0];
    r[9].UD[0] = r[5].UD[0] + r[0].UD[0];
    r[25].UD[0] = r[4].UD[0] + r[0].UD[0];
    do {
        std::array<Vector, 4> points;
        for (uint32_t i = 0; i < 4; ++i) points[i] = Read(memory_, r[8].UL[0] + i * 16);
        const auto packed = PackPoints(points);
        PublishPacked(context, points);
        for (uint32_t i = 0; i < 3; ++i) Write(memory_, r[25].UL[0] + i * 16, packed[i]);
        Advance(r[9], uint32_t(-1));
        Advance(r[8], 64);
        Advance(r[25], 48);
    } while (r[9].SD[0] != 0);
}

void CollisionMath::ProjectMemoryGroups(CpuContext& context, bool sixteen) {
    auto& r = context.cpuRegs.GPR.r;
    auto& v = context.vuRegs.VF;
    const uint32_t output = r[sixteen ? 6 : 7].UL[0];
    r[10].SD[0] = 0x60000000;
    const auto axis = Read(memory_, r[sixteen ? 5 : 6].UL[0]);
    Publish(v[1], axis);
    r[8].UD[0] = r[4].UD[0] + r[0].UD[0];
    r[9].UD[0] = sixteen ? 8 : r[5].UD[0] + r[0].UD[0];
    Vector minimum{0x1p65f, 0x1p65f, 0x1p65f, 0x1p65f};
    Vector maximum{-0x1p65f, -0x1p65f, -0x1p65f, -0x1p65f};
    auto first = ReadGroup(memory_, r[8].UL[0]);
    PointGroup second{};
    if (sixteen) second = ReadGroup(memory_, r[8].UL[0] + 48);
    do {
        const auto a = ProjectGroup(first, axis);
        Publish(context.vuRegs.ACC, a.partial);
        Publish(v[sixteen ? 28 : 5], a.values);
        minimum = Minimum(minimum, a.values);
        maximum = Maximum(maximum, a.values);
        if (sixteen) {
            const auto b = ProjectGroup(second, axis);
            Publish(context.vuRegs.ACC, b.partial);
            Publish(v[29], b.values);
            minimum = Minimum(minimum, b.values);
            maximum = Maximum(maximum, b.values);
        }
        Advance(r[8], sixteen ? 96 : 48);
        Advance(r[9], uint32_t(-1));
        first = ReadGroup(memory_, r[8].UL[0]);
        if (sixteen) second = ReadGroup(memory_, r[8].UL[0] + 48);
    } while (r[9].SD[0] != 0);
    for (size_t i = 0; i < 3; ++i) Publish(v[2 + i], first[i]);
    if (sixteen) for (size_t i = 0; i < 3; ++i) Publish(v[5 + i], second[i]);
    Publish(v[sixteen ? 30 : 6], minimum);
    Publish(v[sixteen ? 31 : 7], maximum);
    PublishRange(context, minimum, maximum);
    memory_.Write32(output, r[8].UL[0]);
    memory_.Write32(output + 4, r[9].UL[0]);
}

void CollisionMath::GroupsRange(CpuContext& context) {
    calls_[4].fetch_add(1, std::memory_order_relaxed);
    ProjectMemoryGroups(context, false);
}

void CollisionMath::SixteenGroupsRange(CpuContext& context) {
    calls_[5].fetch_add(1, std::memory_order_relaxed);
    ProjectMemoryGroups(context, true);
}

void CollisionMath::LoadTriangleHullSupport(CpuContext& context) {
    calls_[6].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& v = context.vuRegs.VF;
    Triangle triangle;
    std::array<Vector, 8> hull;
    for (uint32_t i = 0; i < 3; ++i) triangle[i] = Read(memory_, r[4].UL[0] + i * 16);
    for (uint32_t i = 0; i < 8; ++i) hull[i] = Read(memory_, r[5].UL[0] + i * 16);
    const auto support = BuildHullSupport(triangle, hull);
    std::array<Vector, 4> last_points;
    for (size_t i = 0; i < 4; ++i) {
        last_points[i] = SubtractPoints(triangle[2], hull[4 + i]);
        const auto& previous = v[i == 0 ? 1 : 16 + i];
        std::memcpy(&last_points[i][3], &previous.w, sizeof(float));
    }
    PublishPoint(v[1], last_points[0]);
    for (size_t group = 0; group < 6; ++group)
        for (size_t row = 0; row < 3; ++row) Publish(v[2 + group * 3 + row], support[group][row]);
    for (size_t i = 0; i < 3; ++i) Publish(v[20 + i], triangle[i]);
    for (size_t i = 0; i < 8; ++i) Publish(v[23 + i], hull[i]);
    PublishPacked(context, last_points);
}

void CollisionMath::TriangleHullSupportRange(CpuContext& context) {
    calls_[7].fetch_add(1, std::memory_order_relaxed);
    auto& r = context.cpuRegs.GPR.r;
    auto& v = context.vuRegs.VF;
    const auto axis = Read(memory_, r[4].UL[0]);
    HullSupport support;
    for (size_t group = 0; group < 6; ++group)
        for (size_t row = 0; row < 3; ++row) support[group][row] = Value(v[2 + group * 3 + row]);
    const auto result = ProjectHullSupport(support, axis);
    Publish(context.vuRegs.ACC, result.partial);
    Publish(v[20], result.upper_five);
    Publish(v[21], result.upper_four);
    Publish(v[22], result.lower_three);
    Publish(v[30], result.penultimate);
    Publish(v[31], result.minimum);
    Publish(v[1], result.maximum);
    v[31].z = std::min(result.minimum[2], result.minimum[3]);
    v[1].x = std::max(result.maximum[0], result.maximum[1]);
    v[1].z = std::max(result.maximum[2], result.maximum[3]);
    v[30].x = result.range[0];
    v[31].x = result.range[1];
    Publish(r[8], Value(v[30]));
    Publish(r[9], Value(v[31]));
    memory_.Write32(r[5].UL[0], r[8].UL[0]);
    memory_.Write32(r[5].UL[0] + 4, r[9].UL[0]);
}

CollisionMath::Counters CollisionMath::Counts() const {
    Counters result{};
    for (size_t i = 0; i < calls_.size(); ++i) result.entries[i] = calls_[i].load(std::memory_order_relaxed);
    result.generated_continuations = continuations_->load(std::memory_order_relaxed);
    result.unsupported_inputs = unsupported_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < result.generated_body_calls.size(); ++i)
        result.generated_body_calls[i] = (*generated_body_calls_)[i].load(std::memory_order_relaxed);
    return result;
}

std::string CollisionMath::Census() const {
    constexpr const char* names[] = {"start_edges", "finish_edges", "vertex_differences", "group_points",
        "groups_range", "sixteen_groups_range", "load_support", "support_range"};
    const auto counts = Counts();
    std::string result;
    for (size_t i = 0; i < 8; ++i) {
        if (i != 0) result += ',';
        result += names[i];
        result += '=' + std::to_string(counts.entries[i]);
    }
    result += ",generated_continuations=" + std::to_string(counts.generated_continuations);
    result += ",unsupported_inputs=" + std::to_string(counts.unsupported_inputs);
    for (size_t i = 0; i < counts.generated_body_calls.size(); ++i)
        result += std::string(",generated_body_") + names[i] + '=' + std::to_string(counts.generated_body_calls[i]);
    return result;
}

CollisionMath::~CollisionMath() {
    if (installation_) installation_->store(false, std::memory_order_release);
}

}  // namespace title
