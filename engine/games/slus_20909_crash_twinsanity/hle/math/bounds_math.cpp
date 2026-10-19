#include "games/slus_20909_crash_twinsanity/hle/math/bounds_math.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "games/slus_20909_crash_twinsanity/hle/math/guest_span.h"

namespace title {
namespace {
void Publish(VR_reg& reg, const BoundsMath::Vector& value) {
    std::memcpy(reg.F, value.data(), sizeof(reg.F));
}
void Advance(GPR_reg& reg, uint32_t amount) {
    reg.SD[0] = static_cast<int32_t>(reg.UL[0] + amount);
}
}

const std::array<BoundsMath::Entry, 3>& BoundsMath::Entries() {
    static const std::array<Entry, 3> entries{{
        {0x201400, "triangle"}, {0x201428, "points"}, {0x201460, "transformed_points"}}};
    return entries;
}

void BoundsMath::Include(Box& box, const Vector& point) {
    // Equal and unordered comparisons retain the first operand, matching the
    // generated macro bodies. No arithmetic clamp/denormal flush is applied.
    for (size_t lane = 0; lane < 3; ++lane) {
        if (point[lane] < box.minimum[lane]) box.minimum[lane] = point[lane];
        if (box.maximum[lane] < point[lane]) box.maximum[lane] = point[lane];
    }
}

BoundsMath::Result BoundsMath::Points(const Vector* points, size_t count, const Matrix* transform) {
    if (!points || !count) throw std::invalid_argument("BoundsMath: an initial point is required");
    Result result{};
    for (size_t index = 0; index < count; ++index) {
        result.last_point = points[index];
        if (transform) {
            const auto moved = MathLibrary::EvaluateTransformVector(*transform, points[index]);
            result.last_point = moved.output;
            result.accumulator = moved.accumulator;
        }
        if (!index) result.box = {result.last_point, result.last_point};
        else Include(result.box, result.last_point);
    }
    return result;
}

BoundsMath::Box BoundsMath::Triangle(const Vector& a, const Vector& b, const Vector& c,
                                   float minimum_w, float maximum_w) {
    Box result{a, a};
    Include(result, b);
    Include(result, c);
    result.minimum[3] = minimum_w;
    result.maximum[3] = maximum_w;
    return result;
}

// Spans are validated and copied per aligned quad, as the generated lq/sq
// are: source objects on the mirrored scratchpad stack may straddle its seam.
bool BoundsMath::Range(uint32_t address, uint64_t size) {
    return GuestSpan<ps2r::ipu::GuestMemory>::Mapped(memory_, address, size);
}

BoundsMath::Vector BoundsMath::Read(uint32_t address) {
    Vector result{};
    GuestSpan<ps2r::ipu::GuestMemory>::Read(memory_, address & ~15u, result.data(), 16);
    return result;
}

void BoundsMath::Write(uint32_t address, const Box& box) {
    GuestSpan<ps2r::ipu::GuestMemory>::Write(memory_, address & ~15u, &box, sizeof(box));
}

bool BoundsMath::Triangle(CpuContext& context) {
    const auto& regs = context.cpuRegs.GPR.r;
    const uint32_t output = regs[4].UL[0] & ~15u;
    if (!Range(output, sizeof(Box))) return false;
    for (unsigned reg : {5, 6, 7}) if (!Range(regs[reg].UL[0] & ~15u, 16)) return false;
    const auto a = Read(regs[5].UL[0]), b = Read(regs[6].UL[0]), c = Read(regs[7].UL[0]);
    const auto box = Triangle(a, b, c, context.vuRegs.VF[11].F[3], context.vuRegs.VF[12].F[3]);
    Publish(context.vuRegs.VF[8], a);
    Publish(context.vuRegs.VF[9], b);
    Publish(context.vuRegs.VF[10], c);
    Publish(context.vuRegs.VF[11], box.minimum);
    Publish(context.vuRegs.VF[12], box.maximum);
    Write(output, box);
    return true;
}

bool BoundsMath::Points(CpuContext& context, bool transformed) {
    auto& regs = context.cpuRegs.GPR.r;
    const int32_t requested = regs[5].SL[0];
    // Retail's decrement wraps INT_MIN into a huge loop, outside valid hull
    // ownership. Do not replace that malformed span by the source's first point.
    if (requested == std::numeric_limits<int32_t>::min()) return false;
    const size_t count = requested > 1 ? static_cast<size_t>(requested) : 1;
    const uint32_t input = regs[4].UL[0] & ~15u, output = regs[6].UL[0] & ~15u;
    const uint32_t matrix_address = regs[7].UL[0] & ~15u;
    const uint64_t input_size = static_cast<uint64_t>(count > 1 ? count : 2) * sizeof(Vector);
    if (!Range(input, input_size) || !Range(output, sizeof(Box)) ||
        (transformed && !Range(matrix_address, sizeof(Matrix)))) return false;
    Matrix matrix;
    if (transformed) GuestSpan<ps2r::ipu::GuestMemory>::Read(memory_, matrix_address, matrix.data(), sizeof(matrix));
    std::vector<Vector> points(count);
    GuestSpan<ps2r::ipu::GuestMemory>::Read(memory_, input, points.data(), count * sizeof(Vector));
    const auto look_ahead = Read(input + 16);
    const auto result = Points(points.data(), points.size(), transformed ? &matrix : nullptr);
    // All reads, including inputs aliased by the result, precede publication.
    Advance(regs[4], static_cast<uint32_t>(count * 16));
    regs[5].SD[0] = requested > 1 ? 0 : static_cast<int32_t>(requested - 1);
    Publish(context.vuRegs.VF[1], result.box.minimum);
    Publish(context.vuRegs.VF[2], result.box.maximum);
    Publish(context.vuRegs.VF[3], requested > 1 ? result.last_point : look_ahead);
    if (transformed) {
        for (size_t row = 0; row < 4; ++row) Publish(context.vuRegs.VF[4 + row], matrix[row]);
        Publish(context.vuRegs.ACC, result.accumulator);
    }
    Write(output, result.box);
    return true;
}

bool BoundsMath::Invoke(uint32_t entry, CpuContext& context) {
    if (entry == 0x201400) return Triangle(context);
    if (entry == 0x201428) return Points(context, false);
    if (entry == 0x201460) return Points(context, true);
    return false;
}

}  // namespace title
