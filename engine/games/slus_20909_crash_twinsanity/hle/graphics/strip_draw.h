#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/gs/hle/ir/draw_ir.h"

namespace title {

// Geometry for the four untextured Strip::Draw intents.
// Material/target setup and ordered submission belong to the caller.
class StripDraw {
public:
    static constexpr size_t kMaxVertices = 255;

    struct Position {
        float x = 0, y = 0;
    };
    struct Target {
        int32_t display_width = 0, display_height = 0;
        int32_t offset_x = 0, offset_y = 0;
    };
    enum class Result {
        Ok, InvalidCount, MissingInput, UnsupportedState, InvalidCoordinates,
        InvalidTransform
    };
    using Matrix = std::array<std::array<float, 4>, 4>;
    struct Effects {
        // Column vectors, as used by MathLibrary::EvaluateTransformPoint.
        // A point is (x,y,0,1); the result's x/y are used without division by w.
        const Matrix* transform = nullptr;
        bool tinted = false;
        // The title's neutral tint is RGB=192, A=128, not 255.
        uint32_t tint = 0x80c0c0c0u;
    };

    // state is a complete explicit material/draw snapshot: untextured strip
    // (prim_type 4), context 0/1, and unsigned 12.4 pixel offsets. All its state
    // and identity fields survive; geometry, bounds and vertex hash are replaced.
    // Unused texture vertex attributes are canonical zero, with Q=1.
    //
    // Readable inputs need count elements, may overlap each other/output storage,
    // and are borrowed only during this call. state and out may be the same.
    // Success owns all output vertices; errors leave out unchanged. Count 0..2
    // or an entirely degenerate strip succeeds with empty geometry, which the
    // caller must not submit as a draw. Setup/order effects still belong to it.
    //
    // Source coordinates and full-depth output conversion use round-toward-zero;
    // the caller's rounding mode is restored. Nonfinite coordinates are unsupported.
    static Result Build(const ps2r::gs::DrawIr& state, const Target& target,
                        const Position* positions, const uint32_t* colors,
                        size_t count, ps2r::gs::DrawIr& out);

    // The placed/tinted overloads replace the complete producer's temporary
    // arrays and arithmetic. Tint keeps the source's four separate float
    // products and byte truncation/wrap; it is not an integer /192 shortcut.
    // Transform uses the existing game math primitive and does not execute VU.
    // All inputs (including Effects/Matrix) may alias out's prior storage.
    // Nonfinite positions/matrix elements decline atomically. Rounding and
    // ownership have the same contract as the plain overload.
    static Result Build(const ps2r::gs::DrawIr& state, const Target& target,
                        const Position* positions, const uint32_t* colors,
                        size_t count, const Effects& effects, ps2r::gs::DrawIr& out);

private:
    struct Rounding;
};

}  // namespace title
