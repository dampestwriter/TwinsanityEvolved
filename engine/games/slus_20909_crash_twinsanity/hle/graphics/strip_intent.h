#pragma once

#include "games/slus_20909_crash_twinsanity/hle/graphics/material_state.h"

namespace title {

// Read the four complete retail Strip draw calls into an owned Graphics draw.
// This boundary never reads or creates packets, executes guest functions, or
// observes hardware state. The caller supplies the source-owned environment.
class StripIntent {
public:
    static constexpr uint32_t kPlainAddress = 0x001a9120;
    static constexpr uint32_t kTintedAddress = 0x001a92a0;
    static constexpr uint32_t kPlacedAddress = 0x001a9448;
    static constexpr uint32_t kPlacedTintedAddress = 0x001a9660;

    using Read = MaterialState::Read;
    enum class Result {
        Ok, UnsupportedEntry, InvalidMemory, Empty, UnsupportedMaterial,
        UnsupportedTarget, InvalidGeometry
    };
    struct Output {
        uint32_t bucket = 0;
        uint32_t producer = 0; // the captured entry (k*Address), for attribution
        ps2r::gs::DrawIr draw;
    };

    // entry must be an exact entry above, never an interior continuation.
    // a0 is Strip*, a1 is tint or Matrix4x4*, and a2 is tint for PlacedTinted.
    // Strip fields: material +0, byte count +0x10, positions +0x14, colors +0x18.
    // Captures RAM in physical/KSEG0/KSEG1 only; no MMIO reads are permitted.
    // Positions/colors are word aligned, matrix is quadword aligned. Other
    // valid-but-unsupported cases decline for the original entry to execute.
    //
    // Success owns all geometry and state; no pointer survives the call.
    // Empty certifies the same complete flat material/target/input ownership
    // but contains no triangles. All results other than Ok leave out intact.
    // No-triangle calls need no backend work: each later selected draw carries
    // full material state; retained Begin2D sees its unchanged material cache.
    // The caller must append the result at its bucket predecessor through the
    // ordered Graphics bridge before skipping the original producer.
    static Result Capture(const Read& read, uint32_t entry, uint32_t a0,
                          uint32_t a1, uint32_t a2,
                          const MaterialState::Environment& environment, Output& out);
};

}  // namespace title
