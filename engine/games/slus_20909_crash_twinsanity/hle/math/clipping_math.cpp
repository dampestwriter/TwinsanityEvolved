#include "games/slus_20909_crash_twinsanity/hle/math/clipping_math.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "engine/ee/cpu/ps2_float.h"
#include "games/slus_20909_crash_twinsanity/hle/math/guest_span.h"

namespace title {
namespace {
void Copy(VR_reg& reg, const ClippingMath::Vector& vector) {
    std::memcpy(reg.F, vector.data(), sizeof(reg.F));
}
float Product(float a, float b) {
    volatile float result = ps2_float_clamp(a) * ps2_float_clamp(b);
    return ps2_float_clamp(result);
}
}

const std::array<ClippingMath::Entry, 2>& ClippingMath::Entries() {
    static const std::array<Entry, 2> entries{{
        {0x2014d8, "box_at"}, {0x20157c, "box_or_corners"}}};
    return entries;
}

ClippingMath::Corners ClippingMath::BoxCorners(const Box& box) {
    Corners corners{{box.minimum, box.maximum, box.minimum, box.minimum,
                     box.minimum, box.maximum, box.maximum, box.maximum}};
    for (size_t axis = 0; axis < 3; ++axis) {
        corners[2 + axis][axis] = box.maximum[axis];
        corners[5 + axis][axis] = box.minimum[axis];
    }
    return corners;
}

uint32_t ClippingMath::Outcode(const Vector& point) {
    const float limit = std::fabs(point[3]);
    uint32_t code = 0;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (point[axis] > limit) code |= 1u << (axis * 2);
        if (point[axis] < -limit) code |= 2u << (axis * 2);
    }
    return code;
}

ClippingMath::Result ClippingMath::ClipCorners(
        const Corners& corners, const Matrix& view, const Vector& guard, float translation_scale) {
    Result result{};
    result.matrix = view;
    result.common = 0xfffu;
    for (size_t index = 0; index < corners.size(); ++index) {
        const auto point = MathLibrary::EvaluateTransformPoint(view, corners[index], translation_scale);
        result.clipped[index] = point.output;
        result.accumulator = point.accumulator;
        for (size_t lane = 0; lane < 4; ++lane)
            result.guarded[index][lane] = Product(point.output[lane], guard[lane]);
        const auto code = (Outcode(point.output) << 6) | Outcode(result.guarded[index]);
        result.masks[index] = code;
        result.any |= code;
        result.common &= code;
    }
    return result;
}

ClippingMath::Result ClippingMath::ClipBox(
        const Box& box, const Matrix& view, const Vector& guard, const Matrix* model, float translation_scale) {
    const auto matrix = model ? MathLibrary::EvaluateMatrixProduct(*model, view).output : view;
    return ClipCorners(BoxCorners(box), matrix, guard, translation_scale);
}

// Spans are validated and copied per aligned quad, as the generated lq/sq
// are: the ChunkView on the mirrored scratchpad stack may straddle its seam.
bool ClippingMath::Range(uint32_t address, uint64_t size) {
    return GuestSpan<ps2r::ipu::GuestMemory>::Mapped(memory_, address, size);
}

void ClippingMath::Publish(CpuContext& context, const Result& result) {
    for (size_t i = 0; i < result.clipped.size(); ++i) {
        Copy(context.vuRegs.VF[1 + i], result.clipped[i]);
        Copy(context.vuRegs.VF[17 + i], result.guarded[i]);
    }
    for (size_t row = 0; row < 4; ++row) Copy(context.vuRegs.VF[10 + row], result.matrix[row]);
    Copy(context.vuRegs.ACC, result.accumulator);
    // Exact temporary words of this source ABI. The game tests only the low
    // twelve bits (Result::any/common); no flag pipeline or timing is modeled.
    const uint32_t prior = context.vuRegs.clip_flag;
    uint32_t any = 0, common = 0xffffffffu, last = 0;
    for (size_t i = 0; i < result.masks.size(); ++i) {
        last = result.masks[i];
        if (i == 0) last |= prior << 12;
        else {
            last |= result.masks[i - 1] << 12;
            last |= i == 1 ? prior << 24 : result.masks[i - 2] << 24;
        }
        any |= last;
        common &= last;
    }
    context.vuRegs.clip_flag = last;
    context.cpuRegs.GPR.r[8].UD[0] = last;
    context.cpuRegs.GPR.r[9].UD[0] = any;
    context.cpuRegs.GPR.r[10].UD[0] = common;
    const uint32_t flags[]{any, common};
    GuestSpan<ps2r::ipu::GuestMemory>::Write(memory_, context.cpuRegs.GPR.r[6].UL[0], flags, sizeof(flags));
}

bool ClippingMath::Invoke(uint32_t source_entry, CpuContext& context) {
    if (source_entry != 0x2014d8 && source_entry != 0x20157c && source_entry != kCorners) return false;
    const auto& regs = context.cpuRegs.GPR.r;
    if (!Range(regs[6].UL[0], 8)) return false;
    Matrix view, model;
    Vector guard;
    Result result;
    if (source_entry == kCorners) {
        Corners corners;
        for (size_t i = 0; i < corners.size(); ++i)
            std::memcpy(corners[i].data(), context.vuRegs.VF[1 + i].F, 16);
        for (size_t row = 0; row < 4; ++row)
            std::memcpy(view[row].data(), context.vuRegs.VF[10 + row].F, 16);
        std::memcpy(guard.data(), context.vuRegs.VF[9].F, 16);
        result = ClipCorners(corners, view, guard, context.vuRegs.VF[0].F[3]);
    } else {
        const uint32_t view_address = regs[4].UL[0] & ~15u;
        const uint32_t box_address = regs[5].UL[0] & ~15u;
        const uint32_t model_address = regs[7].UL[0] & ~15u;
        const bool at = source_entry == 0x2014d8;
        if (!Range(view_address, 80) || !Range(box_address, sizeof(Box)) ||
            (at && !Range(model_address, sizeof(Matrix)))) return false;
        using Span = GuestSpan<ps2r::ipu::GuestMemory>;
        Box box{};
        Span::Read(memory_, box_address, &box, sizeof(box));
        Span::Read(memory_, view_address, view.data(), sizeof(view));
        Span::Read(memory_, view_address + 64, guard.data(), sizeof(guard));
        if (at) Span::Read(memory_, model_address, model.data(), sizeof(model));
        result = ClipBox(box, view, guard, at ? &model : nullptr, context.vuRegs.VF[0].F[3]);
        Copy(context.vuRegs.VF[9], guard);
        if (at) {
            // VF24 is subsequently the eighth guarded corner. Other loaded
            // input rows survive and remain observable to generated callers.
            for (size_t row = 0; row < 4; ++row) {
                Copy(context.vuRegs.VF[24 + row], view[row]);
                Copy(context.vuRegs.VF[28 + row], model[row]);
            }
        }
    }
    Publish(context, result);
    return true;
}

}  // namespace title
