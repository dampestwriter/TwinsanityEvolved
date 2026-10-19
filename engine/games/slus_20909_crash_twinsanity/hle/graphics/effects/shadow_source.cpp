#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_source.h"

#include <cmath>
#include <cstring>

#include "engine/ee/mem/scratchpad.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

#include "engine/platform/host/platform.h"

PS2R_FENV_ACCESS_ON

namespace title {
namespace {
using Read = MaterialState::Read;
using Matrix = ShadowGeometry::Matrix;
uint32_t Physical(uint32_t address) {
    const uint32_t segment = address >> 29;
    return segment == 0 || segment == 4 || segment == 5 ? address & 0x1fffffff : 0;
}
// Main RAM, or the 16 KB scratchpad the title's main thread runs its stack on
// (TR entry.cpp:61-62,83): DrawShadowEntry's screen/camera/world matrices are
// its locals, so the default mesh's matrix arguments are 0x7000xxxx addresses.
// The reader serves both; a scratchpad address is passed to it unchanged.
template<class T> bool Value(const Read& read, uint32_t at, T& out) {
    if (!read || (at & 3)) return false;
    if (Scratchpad::Contains(at)) {
        const uint32_t offset = Scratchpad::Offset(at);
        return offset < Scratchpad::kSize && sizeof(T) <= Scratchpad::kSize - offset &&
               read(at, &out, sizeof(T));
    }
    at = Physical(at);
    return at && at <= 0x02000000 && sizeof(T) <= 0x02000000 - at && read(at, &out, sizeof(T));
}
bool Finite(const Matrix& value) {
    for (const auto& row : value)
        for (float lane : row) if (!std::isfinite(lane)) return false;
    return true;
}
ShadowSource::Write Store(uint32_t at, const Matrix& matrix) {
    ShadowSource::Write write{at, std::vector<uint8_t>(sizeof(matrix))};
    std::memcpy(write.bytes.data(), matrix.data(), sizeof(matrix));
    return write;
}
bool Frame(const ShadowDraw::Frame& frame) {
    return frame.width >= 64 && frame.width <= 2048 && !(frame.width % 64) &&
           frame.height && frame.height <= 2048;
}
}
ShadowSource::Result ShadowSource::Begin(const Read& read, const ShadowDraw::Frame& frame,
                                         BeginPlan& out) try {
    if (!Frame(frame)) return Result::UnsupportedSource;
    BeginPlan plan;
    uint32_t target_address = 0;
    Matrix saved{}, camera{};
    std::array<int32_t, 8> target{};
    if (!Value(read, kView, plan.view)) return Result::InvalidMemory;
    plan.view = Physical(plan.view);
    if (!plan.view || (plan.view & 15) || plan.view > 0x01fffeb0 ||
        !Value(read, kTarget, target_address) || !Value(read, target_address, target) ||
        !Value(read, plan.view + 0xc0, saved) || !Value(read, plan.view + 0x80, camera))
        return Result::InvalidMemory;
    if (!Finite(camera)) return Result::InvalidGeometry;
    for (uint32_t address : {kSavedScreen, kShadowCamera, kShadowScreen})
        if (uint64_t(plan.view) < uint64_t(address) + 64 &&
            uint64_t(address) < uint64_t(plan.view) + 0x150) return Result::UnsupportedSource;
    if (target[6] != int32_t(frame.width) || target[7] != int32_t(frame.height) ||
        target[1] < -2048 || target[1] > 2048 || target[2] < -2048 || target[2] > 2048 ||
        target[3] <= 0 || target[3] > 4096 || target[4] <= 0 || target[4] > 4096)
        return Result::UnsupportedSource;
    RoundingGuard rounding;
    const float w = float(target[3] >> 1), h = float(target[4] >> 1);
    const float x = (2048.f - float(target[6] >> 2)) + float(target[1]);
    const float y = (2048.f - float(target[7] >> 2)) + float(target[2]);
    Matrix projection{};
    projection[0][0] = w * 8;
    projection[1][1] = -h * 8;
    projection[2][2] = -8388607.f;
    projection[3] = {(x + w * .5f) * 16, (y + h * .5f) * 16, 8388607.f, 1};
    const auto screen = MathLibrary::EvaluateMatrixProduct(camera, projection).output;
    if (!Finite(screen)) return Result::InvalidGeometry;
    plan.writes = {Store(kSavedScreen, saved), Store(plan.view + 0xc0, screen),
                   Store(kShadowCamera, camera), Store(kShadowScreen, screen)};
    // An aliased source object cannot make this simultaneous publication have
    // different semantics from the source's ordered copies.
    for (size_t i = 0; i < plan.writes.size(); ++i)
        for (size_t j = i + 1; j < plan.writes.size(); ++j)
            if (uint64_t(plan.writes[i].address) < uint64_t(plan.writes[j].address) + 64 &&
                uint64_t(plan.writes[j].address) < uint64_t(plan.writes[i].address) + 64)
                return Result::UnsupportedSource;
    const uint32_t target_start = Physical(target_address);
    for (const auto& write : plan.writes)
        if (uint64_t(write.address) < uint64_t(target_start) + sizeof(target) &&
            uint64_t(target_start) < uint64_t(write.address) + write.bytes.size())
            return Result::UnsupportedSource;
    out = std::move(plan);
    return Result::Ok;
} catch (...) { return Result::InvalidMemory; }

ShadowSource::Result ShadowSource::End(const Read& read, uint32_t original_view, Write& out) try {
    uint32_t view = 0;
    Matrix saved{};
    if (!Value(read, kView, view)) return Result::InvalidMemory;
    view = Physical(view);
    if (!view || (view & 15) || view > 0x01ffff00 ||
        !Value(read, kSavedScreen, saved)) return Result::InvalidMemory;
    if (view != Physical(original_view)) return Result::UnsupportedSource;
    out = Store(view + 0xc0, saved);
    return Result::Ok;
} catch (...) { return Result::InvalidMemory; }

ShadowSource::Result ShadowSource::Mesh(const Read& read, const ShadowAssets& assets,
    const ShadowDraw::Frame& frame, const CpuContext& context, Instance& out, MeshDetail* detail) try {
    MeshDetail seen;
    const auto decline = [&](Result result, MeshCheck check) {
        seen.check = check;
        if (detail) *detail = seen;
        return result;
    };
    seen.screen = context.cpuRegs.GPR.r[5].UL[0];
    seen.camera = context.cpuRegs.GPR.r[6].UL[0];
    if (!Frame(frame)) return decline(Result::UnsupportedSource, MeshCheck::Frame);
    // Quadword-aligned matrices in RAM or on the scratchpad stack.
    if ((context.cpuRegs.GPR.r[5].UL[0] | context.cpuRegs.GPR.r[6].UL[0]) & 15)
        return decline(Result::InvalidMemory, MeshCheck::Matrices);
    Instance value;
    value.object = seen.object = Physical(context.cpuRegs.GPR.r[4].UL[0]);
    std::array<uint32_t, 8> model{};
    std::array<uint32_t, 7> geometry{};
    uint32_t resource = 0, render_view = 0;
    if (!Value(read, value.object, model)) return decline(Result::InvalidMemory, MeshCheck::Object);
    seen.geometry = Physical(model[3]);
    seen.model_id = model[7];
    if (!Value(read, model[3], geometry) ||
        !Value(read, model[2], resource) || resource > 0xfffffff3 ||
        !Value(read, resource + 8, value.material)) return decline(Result::InvalidMemory, MeshCheck::Object);
    // InstanceBlockOwner: no block open from a model-arm writer, no billboard
    // (a turned matrix); NeedsEye is the type-0B shader's answer (1) and only
    // sizes the source instance. One material slot, one submodel.
    if (model[4] || (model[5] & 0xff00) || model[6] != 1 || geometry[2] != 1)
        return decline(Result::UnsupportedSource, MeshCheck::Shape);
    value.eye = model[5] & 0xff;
    value.asset = assets.Find(model[3]);
    if (!value.asset) return decline(Result::UnsupportedSource, MeshCheck::Asset);
    seen.asset_id = value.asset->id;
    if (value.asset->id != geometry[1] || model[7] != geometry[1])
        return decline(Result::UnsupportedSource, MeshCheck::Identity);
    std::array<uint32_t, 11> material{};
    if (!Value(read, value.material, material)) return decline(Result::InvalidMemory, MeshCheck::Material);
    // Source type 0B supplies two constant-colour nodes, no texture/settings.
    // Its vtable, material key and bucket prove ownership without a stored packet.
    if (material[4] != 1 || material[6] != 32 || material[7] || material[8] != 21 ||
        material[10]) return decline(Result::UnsupportedSource, MeshCheck::Material); // foreign active writer
    uint32_t kind = 0, vtable = 0;
    if (material[0] > 0xffffff90 ||
        !Value(read, material[0] + 0x64, kind) || !Value(read, material[0] + 0x6c, vtable))
        return decline(Result::InvalidMemory, MeshCheck::Shader);
    if (kind != 11 || vtable != kShaderVtable) return decline(Result::UnsupportedSource, MeshCheck::Shader);
    if (!Value(read, context.cpuRegs.GPR.r[5].UL[0], value.view.screen) ||
        !Value(read, context.cpuRegs.GPR.r[6].UL[0], value.view.clip) ||
        !Value(read, kView, render_view)) return decline(Result::InvalidMemory, MeshCheck::View);
    render_view = Physical(render_view);
    if (!render_view || render_view > 0x01fffeb0 ||
        !Value(read, render_view + 0x140, value.view.guard)) return decline(Result::InvalidMemory, MeshCheck::View);
    if (!Finite(value.view.screen) || !Finite(value.view.clip))
        return decline(Result::InvalidGeometry, MeshCheck::Geometry);
    for (float lane : value.view.guard)
        if (!std::isfinite(lane)) return decline(Result::InvalidGeometry, MeshCheck::Geometry);
    value.view.offset = {float(2048 - (frame.width >> 2)), float(2048 - (frame.height >> 2))};
    out = std::move(value);
    if (detail) *detail = seen;
    return Result::Ok;
} catch (...) {
    if (detail) detail->check = MeshCheck::Object;
    return Result::InvalidMemory;
}
}  // namespace title
