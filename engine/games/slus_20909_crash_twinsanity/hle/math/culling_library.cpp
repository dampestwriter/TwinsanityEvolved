#include "games/slus_20909_crash_twinsanity/hle/math/culling_library.h"

#include <cstring>
#include <limits>

#include "games/slus_20909_crash_twinsanity/hle/math/culling_planes.h"
#include "games/slus_20909_crash_twinsanity/hle/math/guest_span.h"
#include "games/slus_20909_crash_twinsanity/hle/math/math_library.h"

namespace title {

CullingLibrary::CullingLibrary(ps2r::ipu::GuestMemory& memory)
    : memory_(memory) {}

const std::array<CullingLibrary::Entry, 11>& CullingLibrary::Entries() {
    static const std::array<Entry, 11> entries{{
        {0x1e8778, "setup_chunk", Operation::SetUpChunk},
        {0x1e8910, "setup_portal", Operation::SetUpPortal},
        {0x1f4e50, "load_view", Operation::LoadView},
        {0x1f4fb8, "construct_linked", Operation::ConstructLinked},
        {0x1f5f70, "load_model", Operation::LoadModel},
        {0x1f5f90, "test_cell", Operation::TestCell},
        {0x1f5fe8, "test_box_at", Operation::TestBoxAt},
        {0x1f5fb0, "cell_result", Operation::CellResult},
        {0x1f5f48, "drawn_model_result", Operation::DrawnModelResult},
        {0x1f6028, "mesh_result", Operation::MeshResult},
        {0x1f60a8, "instance_result", Operation::InstanceResult}
    }};
    return entries;
}

// Source objects are validated and copied per aligned 16-byte quad, as the
// generated accesses are: the ChunkView/LinkedChunkView locals live on the
// mirrored scratchpad stack and may straddle its 0x70004000 seam.
bool CullingLibrary::Range(uint32_t address, size_t size, size_t alignment) {
    return address != 0 && address % alignment == 0 &&
        GuestSpan<ps2r::ipu::GuestMemory>::Mapped(memory_, address, size);
}

template<class T> std::optional<T> CullingLibrary::Read(uint32_t address, size_t alignment) {
    if (!Range(address, sizeof(T), alignment)) return std::nullopt;
    T value{};
    GuestSpan<ps2r::ipu::GuestMemory>::Read(memory_, address, &value, sizeof(value));
    return value;
}

template<class T> void CullingLibrary::Write(uint32_t address, const T& value) {
    GuestSpan<ps2r::ipu::GuestMemory>::Write(memory_, address, &value, sizeof(value));
}

void CullingLibrary::ImportLegacy() {
    state_.Reset();
    legacy_imports_.fetch_add(1, std::memory_order_relaxed);
}

bool CullingLibrary::SetUp(CpuContext& context, bool portal) {
    const uint32_t matrices = context.cpuRegs.GPR.r[4].UL[0];
    const uint32_t view = context.cpuRegs.GPR.r[5].UL[0];
    if (!Range(matrices, 0x100) || !Range(view, 0x110)) return false;
    const auto place = Read<CullingMath::Matrix>(context.cpuRegs.GPR.r[6].UL[0]);
    const auto draw = Read<CullingMath::Matrix>(matrices + 0x80);
    const auto renderer = Read<uint32_t>(view + 0xf4, 4);
    if (!place || !draw || !renderer || !Range(*renderer, 0x100)) return false;
    const auto screen = Read<CullingMath::Matrix>(*renderer + 0xc0);
    const auto view_planes = Read<CullingPlanes::Planes>(0x3b4580);
    const auto far_planes = Read<CullingPlanes::Planes>(0x3b45e0);
    if (!screen || !view_planes || !far_planes) return false;
    CullingMath::Frustum planes;
    if (portal) {
        const auto portal_planes = Read<CullingPlanes::Planes>(context.cpuRegs.GPR.r[7].UL[0]);
        if (!portal_planes) return false;
        planes = CullingPlanes::ThroughPortal(*view_planes, *far_planes, *place, *portal_planes);
    } else {
        planes = CullingPlanes::InChunk(*view_planes, *far_planes, *place);
    }
    const auto to_screen = MathLibrary::EvaluateMatrixProduct(*draw, *screen).output;
    const auto inverse = MathLibrary::EvaluateRigidInverse(*draw).output;
    Write(matrices, *place);
    Write(view + 0x100, matrices);
    Write(matrices + 0x40, to_screen);
    Write(matrices + 0xc0, inverse);
    state_.LoadPlanes(planes);
    return true;
}

bool CullingLibrary::LoadView(CpuContext& context) {
    const uint32_t view = context.cpuRegs.GPR.r[4].UL[0];
    if (!Range(view, 0x110)) return false;
    const auto renderer = Read<uint32_t>(view + 0xf4, 4);
    const auto clip = Read<CullingMath::Matrix>(view);
    const auto camera = Read<CullingMath::Vector>(view + 0xe0);
    if (!renderer || !clip || !camera || !Range(*renderer, 0x100)) return false;
    const auto screen = Read<CullingMath::Matrix>(*renderer + 0xc0);
    if (!screen) return false;
    state_.LoadView({*clip, *screen, *camera});
    return true;
}

bool CullingLibrary::ConstructLinked(CpuContext& context) {
    const uint32_t object = context.cpuRegs.GPR.r[4].UL[0];
    if (!Range(object, 0x150) || !state_.Capture().view) return false;
    const auto chunk = Read<CullingMath::Matrix>(context.cpuRegs.GPR.r[5].UL[0]);
    const auto transform = Read<CullingMath::Matrix>(context.cpuRegs.GPR.r[6].UL[0]);
    const auto renderer = Read<uint32_t>(0x309840, 4);
    const auto origin = Read<CullingMath::Vector>(0x3234d0);
    if (!chunk || !transform || !renderer || !origin || !Range(*renderer, 0x150)) return false;
    const auto clip = Read<CullingMath::Matrix>(*renderer + 0x80);
    const auto guard = Read<CullingMath::Vector>(*renderer + 0x140);
    if (!clip || !guard || !state_.LoadLink(*chunk, *transform)) return false;
    // All constructor source effects are owned here. The temporary base vtable
    // has no observer: Reset is replaced by the source field initialization.
    Write(object, *clip);
    Write(object + 0xc0, *guard);
    Write(object + 0xd0, *origin);
    Write(object + 0xf0, uint32_t{0});
    Write(object + 0xf4, *renderer);
    Write(object + 0xf8, uint32_t{0});
    Write(object + 0xfc, uint32_t{0});
    Write(object + 0x104, uint32_t{0x2fbbe0});
    Write(object + 0x110, *chunk);
    context.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(object);
    return true;
}

bool CullingLibrary::Produce(CpuContext& context, Operation operation) {
    const uint32_t input = context.cpuRegs.GPR.r[5].UL[0];
    if (operation == Operation::LoadModel) {
        const auto model = Read<CullingMath::Matrix>(input);
        return model && state_.LoadModel(*model);
    }
    const auto box = Read<CullingMath::Box>(input);
    if (!box) return false;
    if (operation == Operation::TestCell) return state_.TestCell(*box);
    const auto model = Read<CullingMath::Matrix>(context.cpuRegs.GPR.r[6].UL[0]);
    return model && state_.TestBoxAt(*box, *model);
}

uint32_t CullingLibrary::Visibility(const CullingMath::Classification& result) {
    return result.outside != 0 ? 0 : result.clipped != 0 ? 2 : 1;
}

bool CullingLibrary::ReadResult(CpuContext& context, Operation operation) {
    const uint32_t object = context.cpuRegs.GPR.r[4].UL[0];
    if (!Range(object, 0x110)) return false;
    if (operation == Operation::CellResult) {
        const auto result = state_.CellResult();
        if (!result) return false;
        const auto visibility = Visibility(*result);
        Write(object + 0xfc, visibility);
        context.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(visibility);
        return true;
    }
    if (operation == Operation::DrawnModelResult) {
        const auto result = state_.DrawnModelResult();
        if (!result) return false;
        Write(object + 0xf0, result->distance);
        Write(object + 0x80, result->to_screen);
        return true;
    }
    const auto result = operation == Operation::MeshResult ? state_.MeshResult() : state_.InstanceResult();
    if (!result) return false;
    const auto visibility = Visibility(result->visibility);
    Write(object + 0xf0, result->distance);
    Write(object + 0xf8, visibility);
    Write(object + 0xfc, visibility);
    if (result->to_screen) Write(object + 0x80, *result->to_screen);
    if (result->to_clip) Write(object + 0x40, *result->to_clip);
    context.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(visibility);
    return true;
}

bool CullingLibrary::Invoke(Operation operation, CpuContext& context) {
    bool result = false;
    switch (operation) {
        case Operation::SetUpChunk: result = SetUp(context, false); break;
        case Operation::SetUpPortal: result = SetUp(context, true); break;
        case Operation::LoadView: result = LoadView(context); break;
        case Operation::ConstructLinked: result = ConstructLinked(context); break;
        case Operation::LoadModel:
        case Operation::TestCell:
        case Operation::TestBoxAt: result = Produce(context, operation); break;
        case Operation::CellResult:
        case Operation::DrawnModelResult:
        case Operation::MeshResult:
        case Operation::InstanceResult: result = ReadResult(context, operation); break;
    }
    return result;
}

}  // namespace title
