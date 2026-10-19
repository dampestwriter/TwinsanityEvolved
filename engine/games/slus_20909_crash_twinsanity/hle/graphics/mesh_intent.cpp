#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_intent.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
template<class T> bool Read(const MaterialState::Read& read, uint32_t address, T& value) {
    return read && address && uint64_t(address) + sizeof(T) <= uint64_t{1} << 32 &&
           read(address, &value, sizeof(T));
}
bool Finite(const MeshGeometry::Matrix& matrix) {
    for (const auto& row : matrix)
        for (float value : row) if (!std::isfinite(value)) return false;
    return true;
}
}  // namespace

MeshIntent::Result MeshIntent::Capture(const MaterialState::Read& read, uint32_t boundary,
    const CpuContext& context, Output& out) {
    if (boundary != kRigid && boundary != kPlaced && boundary != kDefault)
        return Result::Unsupported;
    Output value{};
    value.object = context.cpuRegs.GPR.r[4].UL[0];
    value.kind = boundary == kRigid ? Kind::Rigid : Kind::Placed;
    value.direct = boundary == kDefault;
    std::array<uint32_t, 8> model{};
    uint32_t parts = 0, mode = 0;
    try {
        if (!Read(read, value.object, model) || model[3] > 0xffffffe0 ||
            !Read(read, model[3] + 8, parts)) return Result::InvalidMemory;
        // Owner.block is zero for a selected family. Existing model work,
        // needsEye, or billboard settings cannot join a typed block.
        if (model[4] || (model[5] & 0xffff) || parts > 64 || model[6] > 64 ||
            model[2] > 0xffffff00) return Result::Unsupported;
        value.geometry = model[3];
        for (uint32_t i = 0; i < std::min(parts, model[6]); ++i) {
            uint32_t reference = 0, material = 0;
            if (!Read(read, model[2] + i * 4, reference) || reference > 0xfffffff7 ||
                !Read(read, reference + 8, material) || !material) return Result::InvalidMemory;
            value.materials.push_back(material);
        }
        const uint32_t argument = context.cpuRegs.GPR.r[5].UL[0];
        if (boundary == kRigid) {
            MeshGeometry::Matrix matrix{}, screen{}, clip{};
            uint32_t render_view = 0;
            mode = context.cpuRegs.GPR.r[9].UL[0];
            if (!Read(read, argument, matrix) ||
                !Read(read, kRenderView, render_view) || render_view > 0xfffffeb0 ||
                !Read(read, render_view + 0xc0, screen)) return Result::InvalidMemory;
            if (!Finite(matrix) || !Finite(screen)) return Result::InvalidGeometry;
            RoundingGuard rounding;
            value.view.screen = MathLibrary::EvaluateMatrixProduct(matrix, screen).output;
            if (mode != 1) {
                if (!Read(read, render_view + 0x80, clip) ||
                    !Read(read, render_view + 0x140, value.view.guard)) return Result::InvalidMemory;
                if (!Finite(clip)) return Result::InvalidGeometry;
                value.view.clip = MathLibrary::EvaluateMatrixProduct(matrix, clip).output;
            }
        } else if (boundary == kPlaced) {
            if (argument > 0xfffffefb ||
                !Read(read, argument + 0x80, value.view.screen) ||
                !Read(read, argument + 0xf8, mode)) return Result::InvalidMemory;
            if (mode != 1 &&
                (!Read(read, argument + 0x40, value.view.clip) ||
                 !Read(read, argument + 0xc0, value.view.guard))) return Result::InvalidMemory;
        } else {
            uint32_t render_view = 0;
            mode = 2;
            if (!Read(read, argument, value.view.screen) ||
                !Read(read, context.cpuRegs.GPR.r[6].UL[0], value.view.clip) ||
                !Read(read, kRenderView, render_view) || render_view > 0xfffffeb0 ||
                !Read(read, render_view + 0x140, value.view.guard)) return Result::InvalidMemory;
        }
    } catch (...) { return Result::InvalidMemory; }
    value.view.clipped = mode != 1;
    if (!Finite(value.view.screen) || !Finite(value.view.clip)) return Result::InvalidGeometry;
    for (float lane : value.view.guard) if (!std::isfinite(lane)) return Result::InvalidGeometry;
    value.size = value.kind == Kind::Rigid ? (value.view.clipped ? 18 : 13)
                                          : (value.view.clipped ? 10 : 5);
    out = std::move(value);
    return Result::Ok;
}
}  // namespace title
