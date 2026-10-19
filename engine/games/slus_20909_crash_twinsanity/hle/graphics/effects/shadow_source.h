#pragma once

#include "engine/ee/cpu/cpu_state.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_assets.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_draw.h"

namespace title {

// Source CPU side effects and default-shadow-mesh arguments. No rendering
// packet, hardware register snapshot or device allocation is read or written.
class ShadowSource {
public:
    static constexpr uint32_t kBegin = 0x001c9048, kEnd = 0x001c9ac0, kMesh = 0x001be9f8;
    static constexpr uint32_t kView = 0x00309840, kTarget = 0x0030983c;
    static constexpr uint32_t kSavedScreen = 0x003bd840;
    static constexpr uint32_t kShadowCamera = 0x003b03f0, kShadowScreen = 0x003b0430;
    static constexpr uint32_t kShaderVtable = ShadowAssets::kShaderVtable;
    struct Write { uint32_t address = 0; std::vector<uint8_t> bytes; };
    struct BeginPlan {
        uint32_t view = 0;
        std::vector<Write> writes;
    };
    struct Instance {
        uint32_t object = 0, material = 0;
        ShadowAssets::Lease asset;
        ShadowGeometry::View view;
        // The mesh's shaders answer NeedsEye (every type-0B shader does): the
        // source placed instance is PlacedEyeSize 11 quadwords, else 10. The
        // type-0B program never reads the eye quadword; the size decides when
        // the source instance block closes (size + PlacedRoom 14 >= capacity 100).
        bool eye = false;
    };
    // Source placed-instance quadwords (TR renderer/models.cpp PlacedClippedSize,
    // PlacedEyeSize, PlacedRoom, BlockCapacity).
    static constexpr uint32_t kClippedSize = 10, kEyeSize = 11, kRoom = 14, kCapacity = 100;
    static uint32_t InstanceSize(const Instance& instance) { return instance.eye ? kEyeSize : kClippedSize; }
    enum class Result { Ok, InvalidMemory, UnsupportedSource, InvalidGeometry };
    // The check of Mesh that declined, in source order: the frame, the matrix
    // arguments, the default mesh object/model/resource words, the mesh shape
    // (open instance block, billboard, one material, one submodel), the owned asset for
    // the mesh's model (Find), the asset/mesh/model identity agreement, the
    // type-0B material words, its shader kind/vtable, the view/guard reads and
    // the finiteness of the captured matrices.
    enum class MeshCheck : uint8_t {
        None, Frame, Matrices, Object, Shape, Asset, Identity, Material, Shader, View, Geometry
    };
    // Attribution of a declined Mesh: which check, and the source words it saw
    // (zero when the decline came before they were read).
    struct MeshDetail {
        MeshCheck check = MeshCheck::None;
        uint32_t object = 0, geometry = 0, model_id = 0, asset_id = 0;
        // The matrix argument addresses (a1 toScreen, a2 toCamera) as passed.
        uint32_t screen = 0, camera = 0;
    };
    // GraphicsRefusals detail code of a declined Mesh: the check in the low
    // byte, the Result in the next.
    static uint32_t Detail(Result result, MeshCheck check) {
        return uint32_t(check) | (uint32_t(result) << 8);
    }
    // Plans all writes before publication; the root commits them atomically on
    // the serialized game owner after reserving the whole source pass position.
    static Result Begin(const MaterialState::Read&, const ShadowDraw::Frame&, BeginPlan&);
    static Result End(const MaterialState::Read&, uint32_t original_view, Write&);
    static Result Mesh(const MaterialState::Read&, const ShadowAssets&, const ShadowDraw::Frame&,
                       const CpuContext&, Instance&, MeshDetail* detail = nullptr);
};

}  // namespace title
