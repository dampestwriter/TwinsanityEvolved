#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/effects/cloth_geometry.h"
#include "games/slus_20909_crash_twinsanity/hle/graphics/mesh_material.h"

namespace title {

class ClothMaterial {
public:
    static constexpr uint32_t kSingleVtable = 0x002fa3d8, kThreeVtable = 0x002fa228;
    struct Output {
        MeshMaterial::Output material;
        ClothGeometry::Waves waves;
    };
    using Result = MeshMaterial::Result;
    // One source-owner read at FlushMaterial, after the game's frame update.
    // Reads ordinary shader objects and copies all wave values. No pointer to
    // their transient allocation survives. The lookup must retain the actual
    // converted texture generation, including pending upload ownership.
    //
    // This captures complete single-pass 17/1A material state. The caller also
    // needs the source-consumer certificate and actual ordered frame binding.
    // Failure leaves out unchanged, emits nothing and does not execute a model.
    static Result Capture(const MaterialState::Read&, uint32_t material,
                          const MaterialState::Environment&, const MeshMaterial::Lookup&,
                          Output& out);
};

}  // namespace title
